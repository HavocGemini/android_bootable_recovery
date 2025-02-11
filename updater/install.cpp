/*
 * Copyright (C) 2009 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "updater/install.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/capability.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include <memory>
#include <string>
#include <vector>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parsedouble.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <applypatch/applypatch.h>
#include <bootloader_message/bootloader_message.h>
#include <cutils/android_reboot.h>
#include <ext4_utils/wipe.h>
#include <openssl/sha.h>
#include <selinux/label.h>
#include <selinux/selinux.h>
#include <tune2fs.h>
#include <ziparchive/zip_archive.h>

#include "edify/expr.h"
#include "mounts.h"

#include "applypatch/applypatch.h"
#include "flashutils/flashutils.h"
#include "install.h"
#ifdef HAVE_LIBTUNE2FS
#include "tune2fs.h"
#endif

#ifdef USE_EXT4
#include "make_ext4fs.h"
#include "wipe.h"
#endif

#include "otautil/ZipUtil.h"
#include "otafault/ota_io.h"
#include "otautil/DirUtil.h"
#include "otautil/error_code.h"
#include "otautil/print_sha1.h"
#include "updater/updater.h"

// Send over the buffer to recovery though the command pipe.
static void uiPrint(State* state, const std::string& buffer) {
  UpdaterInfo* ui = static_cast<UpdaterInfo*>(state->cookie);

  // "line1\nline2\n" will be split into 3 tokens: "line1", "line2" and "".
  // So skip sending empty strings to UI.
  std::vector<std::string> lines = android::base::Split(buffer, "\n");
  for (auto& line : lines) {
    if (!line.empty()) {
      fprintf(ui->cmd_pipe, "ui_print %s\n", line.c_str());
    }
  }

  // On the updater side, we need to dump the contents to stderr (which has
  // been redirected to the log file). Because the recovery will only print
  // the contents to screen when processing pipe command ui_print.
  LOG(INFO) << buffer;
}

static bool is_dir(const std::string& dirpath) {
  struct stat st;
  return stat(dirpath.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// Create all parent directories of name, if necessary.
static bool make_parents(const std::string& name) {
  size_t prev_end = 0;
  while (prev_end < name.size()) {
    size_t next_end = name.find('/', prev_end + 1);
    if (next_end == std::string::npos) {
      break;
    }
    std::string dir_path = name.substr(0, next_end);
    if (!is_dir(dir_path)) {
      int result = mkdir(dir_path.c_str(), 0700);
      if (result != 0) {
        PLOG(ERROR) << "failed to mkdir " << dir_path << " when make parents for " << name;
        return false;
      }

      LOG(INFO) << "created [" << dir_path << "]";
    }
    prev_end = next_end;
  }
  return true;
}

void uiPrintf(State* _Nonnull state, const char* _Nonnull format, ...) {
  std::string error_msg;

  va_list ap;
  va_start(ap, format);
  android::base::StringAppendV(&error_msg, format, ap);
  va_end(ap);

  uiPrint(state, error_msg);
}

// This is the updater side handler for ui_print() in edify script. Contents will be sent over to
// the recovery side for on-screen display.
Value* UIPrintFn(const char* name, State* state, const std::vector<std::unique_ptr<Expr>>& argv) {
  std::vector<std::string> args;
  if (!ReadArgs(state, argv, &args)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s(): Failed to parse the argument(s)", name);
  }

  std::string buffer = android::base::Join(args, "");
  uiPrint(state, buffer);
  return StringValue(buffer);
}

// package_extract_file(package_file[, dest_file])
//   Extracts a single package_file from the update package and writes it to dest_file,
//   overwriting existing files if necessary. Without the dest_file argument, returns the
//   contents of the package file as a binary blob.
Value* PackageExtractFileFn(const char* name, State* state,
                            const std::vector<std::unique_ptr<Expr>>& argv) {
  if (argv.size() < 1 || argv.size() > 2) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects 1 or 2 args, got %zu", name,
                      argv.size());
  }

  if (argv.size() == 2) {
    // The two-argument version extracts to a file.

    std::vector<std::string> args;
    if (!ReadArgs(state, argv, &args)) {
      return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse %zu args", name,
                        argv.size());
    }
    const std::string& zip_path = args[0];
    const std::string& dest_path = args[1];

    ZipArchiveHandle za = static_cast<UpdaterInfo*>(state->cookie)->package_zip;
    ZipString zip_string_path(zip_path.c_str());
    ZipEntry entry;
    if (FindEntry(za, zip_string_path, &entry) != 0) {
      LOG(ERROR) << name << ": no " << zip_path << " in package";
      return StringValue("");
    }

    unique_fd fd(TEMP_FAILURE_RETRY(
        ota_open(dest_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR)));
    if (fd == -1) {
      PLOG(ERROR) << name << ": can't open " << dest_path << " for write";
      return StringValue("");
    }

    bool success = true;
    int32_t ret = ExtractEntryToFile(za, &entry, fd);
    if (ret != 0) {
      LOG(ERROR) << name << ": Failed to extract entry \"" << zip_path << "\" ("
                 << entry.uncompressed_length << " bytes) to \"" << dest_path
                 << "\": " << ErrorCodeString(ret);
      success = false;
    }
    if (ota_fsync(fd) == -1) {
      PLOG(ERROR) << "fsync of \"" << dest_path << "\" failed";
      success = false;
    }
    if (ota_close(fd) == -1) {
      PLOG(ERROR) << "close of \"" << dest_path << "\" failed";
      success = false;
    }

    return StringValue(success ? "t" : "");
  } else {
    // The one-argument version returns the contents of the file as the result.

    std::vector<std::string> args;
    if (!ReadArgs(state, argv, &args)) {
      return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse %zu args", name,
                        argv.size());
    }
    const std::string& zip_path = args[0];

    ZipArchiveHandle za = static_cast<UpdaterInfo*>(state->cookie)->package_zip;
    ZipString zip_string_path(zip_path.c_str());
    ZipEntry entry;
    if (FindEntry(za, zip_string_path, &entry) != 0) {
      return ErrorAbort(state, kPackageExtractFileFailure, "%s(): no %s in package", name,
                        zip_path.c_str());
    }

    std::string buffer;
    buffer.resize(entry.uncompressed_length);

    int32_t ret =
        ExtractToMemory(za, &entry, reinterpret_cast<uint8_t*>(&buffer[0]), buffer.size());
    if (ret != 0) {
      return ErrorAbort(state, kPackageExtractFileFailure,
                        "%s: Failed to extract entry \"%s\" (%zu bytes) to memory: %s", name,
                        zip_path.c_str(), buffer.size(), ErrorCodeString(ret));
    }

    return new Value(VAL_BLOB, buffer);
  }
}

// apply_patch(src_file, tgt_file, tgt_sha1, tgt_size, patch1_sha1, patch1_blob, [...])
//   Applies a binary patch to the src_file to produce the tgt_file. If the desired target is the
//   same as the source, pass "-" for tgt_file. tgt_sha1 and tgt_size are the expected final SHA1
//   hash and size of the target file. The remaining arguments must come in pairs: a SHA1 hash (a
//   40-character hex string) and a blob. The blob is the patch to be applied when the source
//   file's current contents have the given SHA1.
//
//   The patching is done in a safe manner that guarantees the target file either has the desired
//   SHA1 hash and size, or it is untouched -- it will not be left in an unrecoverable intermediate
//   state. If the process is interrupted during patching, the target file may be in an intermediate
//   state; a copy exists in the cache partition so restarting the update can successfully update
//   the file.
Value* ApplyPatchFn(const char* name, State* state,
                    const std::vector<std::unique_ptr<Expr>>& argv) {
  if (argv.size() < 6 || (argv.size() % 2) == 1) {
    return ErrorAbort(state, kArgsParsingFailure,
                      "%s(): expected at least 6 args and an "
                      "even number, got %zu",
                      name, argv.size());
  }

  std::vector<std::string> args;
  if (!ReadArgs(state, argv, &args, 0, 4)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse the argument(s)", name);
  }
  const std::string& source_filename = args[0];
  const std::string& target_filename = args[1];
  const std::string& target_sha1 = args[2];
  const std::string& target_size_str = args[3];

  size_t target_size;
  if (!android::base::ParseUint(target_size_str.c_str(), &target_size)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s(): can't parse \"%s\" as byte count", name,
                      target_size_str.c_str());
  }

  int patchcount = (argv.size() - 4) / 2;
  std::vector<std::unique_ptr<Value>> arg_values;
  if (!ReadValueArgs(state, argv, &arg_values, 4, argv.size() - 4)) {
    return nullptr;
  }

  for (int i = 0; i < patchcount; ++i) {
    if (arg_values[i * 2]->type != VAL_STRING) {
      return ErrorAbort(state, kArgsParsingFailure, "%s(): sha-1 #%d is not string", name, i * 2);
    }
    if (arg_values[i * 2 + 1]->type != VAL_BLOB) {
      return ErrorAbort(state, kArgsParsingFailure, "%s(): patch #%d is not blob", name, i * 2 + 1);
    }
  }

  std::vector<std::string> patch_sha_str;
  std::vector<std::unique_ptr<Value>> patches;
  for (int i = 0; i < patchcount; ++i) {
    patch_sha_str.push_back(arg_values[i * 2]->data);
    patches.push_back(std::move(arg_values[i * 2 + 1]));
  }

  int result = applypatch(source_filename.c_str(), target_filename.c_str(), target_sha1.c_str(),
                          target_size, patch_sha_str, patches, nullptr);

  return StringValue(result == 0 ? "t" : "");
}

// apply_patch_check(filename, [sha1, ...])
//   Returns true if the contents of filename or the temporary copy in the cache partition (if
//   present) have a SHA-1 checksum equal to one of the given sha1 values. sha1 values are
//   specified as 40 hex digits. This function differs from sha1_check(read_file(filename),
//   sha1 [, ...]) in that it knows to check the cache partition copy, so apply_patch_check() will
//   succeed even if the file was corrupted by an interrupted apply_patch() update.
Value* ApplyPatchCheckFn(const char* name, State* state,
                         const std::vector<std::unique_ptr<Expr>>& argv) {
  if (argv.size() < 1) {
    return ErrorAbort(state, kArgsParsingFailure, "%s(): expected at least 1 arg, got %zu", name,
                      argv.size());
  }

  std::vector<std::string> args;
  if (!ReadArgs(state, argv, &args, 0, 1)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse the argument(s)", name);
  }
  const std::string& filename = args[0];

  std::vector<std::string> sha1s;
  if (argv.size() > 1 && !ReadArgs(state, argv, &sha1s, 1, argv.size() - 1)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse the argument(s)", name);
  }
  int result = applypatch_check(filename.c_str(), sha1s);

  return StringValue(result == 0 ? "t" : "");
}

// sha1_check(data)
//    to return the sha1 of the data (given in the format returned by
//    read_file).
//
// sha1_check(data, sha1_hex, [sha1_hex, ...])
//    returns the sha1 of the file if it matches any of the hex
//    strings passed, or "" if it does not equal any of them.
//
Value* Sha1CheckFn(const char* name, State* state, const std::vector<std::unique_ptr<Expr>>& argv) {
  if (argv.size() < 1) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects at least 1 arg", name);
  }

  std::vector<std::unique_ptr<Value>> args;
  if (!ReadValueArgs(state, argv, &args)) {
    return nullptr;
  }

  if (args[0]->type == VAL_INVALID) {
    return StringValue("");
  }
  uint8_t digest[SHA_DIGEST_LENGTH];
  SHA1(reinterpret_cast<const uint8_t*>(args[0]->data.c_str()), args[0]->data.size(), digest);

  if (argv.size() == 1) {
    return StringValue(print_sha1(digest));
  }

  for (size_t i = 1; i < argv.size(); ++i) {
    uint8_t arg_digest[SHA_DIGEST_LENGTH];
    if (args[i]->type != VAL_STRING) {
      LOG(ERROR) << name << "(): arg " << i << " is not a string; skipping";
    } else if (ParseSha1(args[i]->data.c_str(), arg_digest) != 0) {
      // Warn about bad args and skip them.
      LOG(ERROR) << name << "(): error parsing \"" << args[i]->data << "\" as sha-1; skipping";
    } else if (memcmp(digest, arg_digest, SHA_DIGEST_LENGTH) == 0) {
      // Found a match.
      return args[i].release();
    }
  }

  // Didn't match any of the hex strings; return false.
  return StringValue("");
}

// mount(fs_type, partition_type, location, mount_point)
// mount(fs_type, partition_type, location, mount_point, mount_options)

//    fs_type="ext4"   partition_type="EMMC"    location=device
Value* MountFn(const char* name, State* state, const std::vector<std::unique_ptr<Expr>>& argv) {
  if (argv.size() != 4 && argv.size() != 5) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects 4-5 args, got %zu", name,
                      argv.size());
  }

  std::vector<std::string> args;
  if (!ReadArgs(state, argv, &args)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse the argument(s)", name);
  }
  const std::string& fs_type = args[0];
  const std::string& partition_type = args[1];
  const std::string& location = args[2];
  const std::string& mount_point = args[3];
  std::string mount_options;

  if (argv.size() == 5) {
    mount_options = args[4];
  }

  if (fs_type.empty()) {
    return ErrorAbort(state, kArgsParsingFailure, "fs_type argument to %s() can't be empty", name);
  }
  if (partition_type.empty()) {
    return ErrorAbort(state, kArgsParsingFailure, "partition_type argument to %s() can't be empty",
                      name);
  }
  if (location.empty()) {
    return ErrorAbort(state, kArgsParsingFailure, "location argument to %s() can't be empty", name);
  }
  if (mount_point.empty()) {
    return ErrorAbort(state, kArgsParsingFailure, "mount_point argument to %s() can't be empty",
                      name);
  }

  {
    char* secontext = nullptr;

    if (sehandle) {
      selabel_lookup(sehandle, &secontext, mount_point.c_str(), 0755);
      setfscreatecon(secontext);
    }

    mkdir(mount_point.c_str(), 0755);

    if (secontext) {
      freecon(secontext);
      setfscreatecon(nullptr);
    }
  }

  if (mount(location.c_str(), mount_point.c_str(), fs_type.c_str(),
            MS_NOATIME | MS_NODEV | MS_NODIRATIME, mount_options.c_str()) < 0) {
    uiPrintf(state, "%s: Failed to mount %s at %s: %s", name, location.c_str(), mount_point.c_str(),
             strerror(errno));
    return StringValue("");
  }

  return StringValue(mount_point);
}

// is_mounted(mount_point)
Value* IsMountedFn(const char* name, State* state, const std::vector<std::unique_ptr<Expr>>& argv) {
  if (argv.size() != 1) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects 1 arg, got %zu", name, argv.size());
  }

  std::vector<std::string> args;
  if (!ReadArgs(state, argv, &args)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse the argument(s)", name);
  }
  const std::string& mount_point = args[0];
  if (mount_point.empty()) {
    return ErrorAbort(state, kArgsParsingFailure,
                      "mount_point argument to unmount() can't be empty");
  }

  scan_mounted_volumes();
  MountedVolume* vol = find_mounted_volume_by_mount_point(mount_point.c_str());
  if (vol == nullptr) {
    return StringValue("");
  }

  return StringValue(mount_point);
}

Value* UnmountFn(const char* name, State* state, const std::vector<std::unique_ptr<Expr>>& argv) {
  if (argv.size() != 1) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects 1 arg, got %zu", name, argv.size());
  }
  std::vector<std::string> args;
  if (!ReadArgs(state, argv, &args)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse the argument(s)", name);
  }
  const std::string& mount_point = args[0];
  if (mount_point.empty()) {
    return ErrorAbort(state, kArgsParsingFailure,
                      "mount_point argument to unmount() can't be empty");
  }

  scan_mounted_volumes();
  MountedVolume* vol = find_mounted_volume_by_mount_point(mount_point.c_str());
  if (vol == nullptr) {
    uiPrintf(state, "Failed to unmount %s: No such volume", mount_point.c_str());
    return nullptr;
  } else {
    int ret = unmount_mounted_volume(vol);
    if (ret != 0) {
      uiPrintf(state, "Failed to unmount %s: %s", mount_point.c_str(), strerror(errno));
    }
  }

  return StringValue(mount_point);
}

static int exec_cmd(const char* path, char* const argv[]) {
  pid_t child;
  if ((child = vfork()) == 0) {
    execv(path, argv);
    _exit(EXIT_FAILURE);
  }

  int status;
  waitpid(child, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    LOG(ERROR) << path << " failed with status " << WEXITSTATUS(status);
  }
  return WEXITSTATUS(status);
}

// format(fs_type, partition_type, location, fs_size, mount_point)
//
//    fs_type="ext4"  partition_type="EMMC"  location=device  fs_size=<bytes> mount_point=<location>
//    fs_type="f2fs"  partition_type="EMMC"  location=device  fs_size=<bytes> mount_point=<location>
//    if fs_size == 0, then make fs uses the entire partition.
//    if fs_size > 0, that is the size to use
//    if fs_size < 0, then reserve that many bytes at the end of the partition (not for "f2fs")
Value* FormatFn(const char* name, State* state, const std::vector<std::unique_ptr<Expr>>& argv) {
  if (argv.size() != 5) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects 5 args, got %zu", name,
                      argv.size());
  }

  std::vector<std::string> args;
  if (!ReadArgs(state, argv, &args)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse the argument(s)", name);
  }
  const std::string& fs_type = args[0];
  const std::string& partition_type = args[1];
  const std::string& location = args[2];
  const std::string& fs_size = args[3];
  const std::string& mount_point = args[4];

  if (fs_type.empty()) {
    return ErrorAbort(state, kArgsParsingFailure, "fs_type argument to %s() can't be empty", name);
  }
  if (partition_type.empty()) {
    return ErrorAbort(state, kArgsParsingFailure, "partition_type argument to %s() can't be empty",
                      name);
  }
  if (location.empty()) {
    return ErrorAbort(state, kArgsParsingFailure, "location argument to %s() can't be empty", name);
  }
  if (mount_point.empty()) {
    return ErrorAbort(state, kArgsParsingFailure, "mount_point argument to %s() can't be empty",
                      name);
  }

  int64_t size;
  if (!android::base::ParseInt(fs_size, &size)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s: failed to parse int in %s", name,
                      fs_size.c_str());
  }

  if (fs_type == "ext4") {
    const char* mke2fs_argv[] = { "/sbin/mke2fs_static", "-t",    "ext4", "-b", "4096",
                                  location.c_str(),      nullptr, nullptr };
    std::string size_str;
    if (size != 0) {
      size_str = std::to_string(size / 4096LL);
      mke2fs_argv[6] = size_str.c_str();
    }

    int status = exec_cmd(mke2fs_argv[0], const_cast<char**>(mke2fs_argv));
    if (status != 0) {
      LOG(ERROR) << name << ": mke2fs failed (" << status << ") on " << location;
      return StringValue("");
    }

    const char* e2fsdroid_argv[] = { "/sbin/e2fsdroid_static", "-e",   "-a", mount_point.c_str(),
                                     location.c_str(),         nullptr };
    status = exec_cmd(e2fsdroid_argv[0], const_cast<char**>(e2fsdroid_argv));
    if (status != 0) {
      LOG(ERROR) << name << ": e2fsdroid failed (" << status << ") on " << location;
      return StringValue("");
    }
    return StringValue(location);
  } else if (fs_type == "f2fs") {
    if (size < 0) {
      LOG(ERROR) << name << ": fs_size can't be negative for f2fs: " << fs_size;
      return StringValue("");
    }
    std::string num_sectors = std::to_string(size / 512);

    const char* f2fs_path = "/sbin/mkfs.f2fs";
    const char* f2fs_argv[] = { "mkfs.f2fs",
                                "-d1",
                                "-f",
                                "-O", "encrypt",
                                "-O", "quota",
                                "-O", "verity",
                                "-w", "512",
                                location.c_str(),
                                (size < 512) ? nullptr : num_sectors.c_str(),
                                nullptr };
    int status = exec_cmd(f2fs_path, const_cast<char**>(f2fs_argv));
    if (status != 0) {
      LOG(ERROR) << name << ": mkfs.f2fs failed (" << status << ") on " << location;
      return StringValue("");
    }

    const char* sload_argv[] = { "/sbin/sload.f2fs", "-t", mount_point.c_str(), location.c_str(),
                                 nullptr };
    status = exec_cmd(sload_argv[0], const_cast<char**>(sload_argv));
    if (status != 0) {
      LOG(ERROR) << name << ": sload.f2fs failed (" << status << ") on " << location;
      return StringValue("");
    }

    return StringValue(location);
  } else {
    LOG(ERROR) << name << ": unsupported fs_type \"" << fs_type << "\" partition_type \""
               << partition_type << "\"";
  }

  return nullptr;
}

// rename(src_name, dst_name)
//   Renames src_name to dst_name. It automatically creates the necessary directories for dst_name.
//   Example: rename("system/app/Hangouts/Hangouts.apk", "system/priv-app/Hangouts/Hangouts.apk")
Value* RenameFn(const char* name, State* state, const std::vector<std::unique_ptr<Expr>>& argv) {
  if (argv.size() != 2) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects 2 args, got %zu", name,
                      argv.size());
  }

  std::vector<std::string> args;
  if (!ReadArgs(state, argv, &args)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse the argument(s)", name);
  }
  const std::string& src_name = args[0];
  const std::string& dst_name = args[1];

  if (src_name.empty()) {
    return ErrorAbort(state, kArgsParsingFailure, "src_name argument to %s() can't be empty", name);
  }
  if (dst_name.empty()) {
    return ErrorAbort(state, kArgsParsingFailure, "dst_name argument to %s() can't be empty", name);
  }
  if (!make_parents(dst_name)) {
    return ErrorAbort(state, kFileRenameFailure, "Creating parent of %s failed, error %s",
                      dst_name.c_str(), strerror(errno));
  } else if (access(dst_name.c_str(), F_OK) == 0 && access(src_name.c_str(), F_OK) != 0) {
    // File was already moved
    return StringValue(dst_name);
  } else if (rename(src_name.c_str(), dst_name.c_str()) != 0) {
    return ErrorAbort(state, kFileRenameFailure, "Rename of %s to %s failed, error %s",
                      src_name.c_str(), dst_name.c_str(), strerror(errno));
  }

  return StringValue(dst_name);
}

// delete([filename, ...])
//   Deletes all the filenames listed. Returns the number of files successfully deleted.
//
// delete_recursive([dirname, ...])
//   Recursively deletes dirnames and all their contents. Returns the number of directories
//   successfully deleted.
Value* DeleteFn(const char* name, State* state, const std::vector<std::unique_ptr<Expr>>& argv) {
  std::vector<std::string> paths;
  if (!ReadArgs(state, argv, &paths)) {
    return nullptr;
  }

  bool recursive = (strcmp(name, "delete_recursive") == 0);

  int success = 0;
  for (const auto& path : paths) {
    if ((recursive ? dirUnlinkHierarchy(path.c_str()) : unlink(path.c_str())) == 0) {
      ++success;
    }
  }

  return StringValue(std::to_string(success));
}


Value* ShowProgressFn(const char* name, State* state,
                      const std::vector<std::unique_ptr<Expr>>& argv) {
  if (argv.size() != 2) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects 2 args, got %zu", name,
                      argv.size());
  }

  std::vector<std::string> args;
  if (!ReadArgs(state, argv, &args)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse the argument(s)", name);
  }
  const std::string& frac_str = args[0];
  const std::string& sec_str = args[1];

  double frac;
  if (!android::base::ParseDouble(frac_str.c_str(), &frac)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s: failed to parse double in %s", name,
                      frac_str.c_str());
  }
  int sec;
  if (!android::base::ParseInt(sec_str.c_str(), &sec)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s: failed to parse int in %s", name,
                      sec_str.c_str());
  }

  UpdaterInfo* ui = static_cast<UpdaterInfo*>(state->cookie);
  fprintf(ui->cmd_pipe, "progress %f %d\n", frac, sec);

  return StringValue(frac_str);
}

Value* SetProgressFn(const char* name, State* state,
                     const std::vector<std::unique_ptr<Expr>>& argv) {
  if (argv.size() != 1) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects 1 arg, got %zu", name, argv.size());
  }

  std::vector<std::string> args;
  if (!ReadArgs(state, argv, &args)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse the argument(s)", name);
  }
  const std::string& frac_str = args[0];

  double frac;
  if (!android::base::ParseDouble(frac_str.c_str(), &frac)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s: failed to parse double in %s", name,
                      frac_str.c_str());
  }

  UpdaterInfo* ui = static_cast<UpdaterInfo*>(state->cookie);
  fprintf(ui->cmd_pipe, "set_progress %f\n", frac);

  return StringValue(frac_str);
}

// package_extract_dir(package_dir, dest_dir)
//   Extracts all files from the package underneath package_dir and writes them to the
//   corresponding tree beneath dest_dir. Any existing files are overwritten.
//   Example: package_extract_dir("system", "/system")
//
//   Note: package_dir needs to be a relative path; dest_dir needs to be an absolute path.
Value* PackageExtractDirFn(const char* name, State* state,
                           const std::vector<std::unique_ptr<Expr>>&argv) {
  if (argv.size() != 2) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects 2 args, got %zu", name,
                      argv.size());
  }

  std::vector<std::string> args;
  if (!ReadArgs(state, argv, &args)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse the argument(s)", name);
  }
  const std::string& zip_path = args[0];
  const std::string& dest_path = args[1];

  ZipArchiveHandle za = static_cast<UpdaterInfo*>(state->cookie)->package_zip;

  // To create a consistent system image, never use the clock for timestamps.
  constexpr struct utimbuf timestamp = { 1217592000, 1217592000 };  // 8/1/2008 default

  bool success = ExtractPackageRecursive(za, zip_path, dest_path, &timestamp, sehandle);

  return StringValue(success ? "t" : "");
}

// symlink(target, [src1, src2, ...])
//   Creates all sources as symlinks to target. It unlinks any previously existing src1, src2, etc
//   before creating symlinks.
Value* SymlinkFn(const char* name, State* state, const std::vector<std::unique_ptr<Expr>>& argv) {
  if (argv.size() == 0) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects 1+ args, got %zu", name, argv.size());
  }
  std::string target;
  if (!Evaluate(state, argv[0], &target)) {
    return nullptr;
  }

  std::vector<std::string> srcs;
  if (!ReadArgs(state, argv, &srcs, 1, argv.size())) {
    return ErrorAbort(state, kArgsParsingFailure, "%s(): Failed to parse the argument(s)", name);
  }

  size_t bad = 0;
  for (const auto& src : srcs) {
    if (unlink(src.c_str()) == -1 && errno != ENOENT) {
      PLOG(ERROR) << name << ": failed to remove " << src;
      ++bad;
    } else if (!make_parents(src)) {
      LOG(ERROR) << name << ": failed to symlink " << src << " to " << target
                 << ": making parents failed";
      ++bad;
    } else if (symlink(target.c_str(), src.c_str()) == -1) {
      PLOG(ERROR) << name << ": failed to symlink " << src << " to " << target;
      ++bad;
    }
  }
  if (bad != 0) {
    return ErrorAbort(state, kSymlinkFailure, "%s: Failed to create %zu symlink(s)", name, bad);
  }
  return StringValue("t");
}

struct perm_parsed_args {
  bool has_uid;
  uid_t uid;
  bool has_gid;
  gid_t gid;
  bool has_mode;
  mode_t mode;
  bool has_fmode;
  mode_t fmode;
  bool has_dmode;
  mode_t dmode;
  bool has_selabel;
  const char* selabel;
  bool has_capabilities;
  uint64_t capabilities;
};

static struct perm_parsed_args ParsePermArgs(State * state,
                                             const std::vector<std::string>& args) {
  struct perm_parsed_args parsed;
  int bad = 0;
  static int max_warnings = 20;

  memset(&parsed, 0, sizeof(parsed));

  for (size_t i = 1; i < args.size(); i += 2) {
    if (args[i] == "uid") {
      int64_t uid;
      if (sscanf(args[i + 1].c_str(), "%" SCNd64, &uid) == 1) {
        parsed.uid = uid;
        parsed.has_uid = true;
      } else {
        uiPrintf(state, "ParsePermArgs: invalid UID \"%s\"\n", args[i + 1].c_str());
        bad++;
      }
      continue;
    }
    if (args[i] == "gid") {
      int64_t gid;
      if (sscanf(args[i + 1].c_str(), "%" SCNd64, &gid) == 1) {
        parsed.gid = gid;
        parsed.has_gid = true;
      } else {
        uiPrintf(state, "ParsePermArgs: invalid GID \"%s\"\n", args[i + 1].c_str());
        bad++;
      }
      continue;
    }
    if (args[i] == "mode") {
      int32_t mode;
      if (sscanf(args[i + 1].c_str(), "%" SCNi32, &mode) == 1) {
        parsed.mode = mode;
        parsed.has_mode = true;
      } else {
        uiPrintf(state, "ParsePermArgs: invalid mode \"%s\"\n", args[i + 1].c_str());
        bad++;
      }
      continue;
    }
    if (args[i] == "dmode") {
      int32_t mode;
      if (sscanf(args[i + 1].c_str(), "%" SCNi32, &mode) == 1) {
        parsed.dmode = mode;
        parsed.has_dmode = true;
      } else {
        uiPrintf(state, "ParsePermArgs: invalid dmode \"%s\"\n", args[i + 1].c_str());
        bad++;
      }
      continue;
    }
    if (args[i] == "fmode") {
      int32_t mode;
      if (sscanf(args[i + 1].c_str(), "%" SCNi32, &mode) == 1) {
        parsed.fmode = mode;
        parsed.has_fmode = true;
      } else {
        uiPrintf(state, "ParsePermArgs: invalid fmode \"%s\"\n", args[i + 1].c_str());
        bad++;
      }
      continue;
    }
    if (args[i] == "capabilities") {
      int64_t capabilities;
      if (sscanf(args[i + 1].c_str(), "%" SCNi64, &capabilities) == 1) {
        parsed.capabilities = capabilities;
        parsed.has_capabilities = true;
      } else {
        uiPrintf(state, "ParsePermArgs: invalid capabilities \"%s\"\n", args[i + 1].c_str());
        bad++;
      }
      continue;
    }
    if (args[i] == "selabel") {
      if (!args[i + 1].empty()) {
        parsed.selabel = args[i + 1].c_str();
        parsed.has_selabel = true;
      } else {
        uiPrintf(state, "ParsePermArgs: invalid selabel \"%s\"\n", args[i + 1].c_str());
        bad++;
      }
      continue;
    }
    if (max_warnings != 0) {
      printf("ParsedPermArgs: unknown key \"%s\", ignoring\n", args[i].c_str());
      max_warnings--;
      if (max_warnings == 0) {
        LOG(INFO) << "ParsedPermArgs: suppressing further warnings";
      }
    }
  }
  return parsed;
}

static int ApplyParsedPerms(State* state, const char* filename, const struct stat* statptr,
                            struct perm_parsed_args parsed) {
  int bad = 0;

  if (parsed.has_selabel) {
    if (lsetfilecon(filename, parsed.selabel) != 0) {
      uiPrintf(state, "ApplyParsedPerms: lsetfilecon of %s to %s failed: %s\n", filename,
               parsed.selabel, strerror(errno));
      bad++;
    }
  }

  /* ignore symlinks */
  if (S_ISLNK(statptr->st_mode)) {
    return bad;
  }

  if (parsed.has_uid) {
    if (chown(filename, parsed.uid, -1) < 0) {
      uiPrintf(state, "ApplyParsedPerms: chown of %s to %d failed: %s\n", filename, parsed.uid,
               strerror(errno));
      bad++;
    }
  }

  if (parsed.has_gid) {
    if (chown(filename, -1, parsed.gid) < 0) {
      uiPrintf(state, "ApplyParsedPerms: chgrp of %s to %d failed: %s\n", filename, parsed.gid,
               strerror(errno));
      bad++;
    }
  }

  if (parsed.has_mode) {
    if (chmod(filename, parsed.mode) < 0) {
      uiPrintf(state, "ApplyParsedPerms: chmod of %s to %d failed: %s\n", filename, parsed.mode,
               strerror(errno));
      bad++;
    }
  }

  if (parsed.has_dmode && S_ISDIR(statptr->st_mode)) {
    if (chmod(filename, parsed.dmode) < 0) {
      uiPrintf(state, "ApplyParsedPerms: chmod of %s to %d failed: %s\n", filename, parsed.dmode,
               strerror(errno));
      bad++;
    }
  }

  if (parsed.has_fmode && S_ISREG(statptr->st_mode)) {
    if (chmod(filename, parsed.fmode) < 0) {
      uiPrintf(state, "ApplyParsedPerms: chmod of %s to %d failed: %s\n", filename, parsed.fmode,
               strerror(errno));
      bad++;
    }
  }

  if (parsed.has_capabilities && S_ISREG(statptr->st_mode)) {
    if (parsed.capabilities == 0) {
      if ((removexattr(filename, XATTR_NAME_CAPS) == -1) && (errno != ENODATA)) {
        // Report failure unless it's ENODATA (attribute not set)
        uiPrintf(state, "ApplyParsedPerms: removexattr of %s to %" PRIx64 " failed: %s\n", filename,
                 parsed.capabilities, strerror(errno));
        bad++;
      }
    } else {
      struct vfs_cap_data cap_data;
      memset(&cap_data, 0, sizeof(cap_data));
      cap_data.magic_etc = VFS_CAP_REVISION | VFS_CAP_FLAGS_EFFECTIVE;
      cap_data.data[0].permitted = (uint32_t)(parsed.capabilities & 0xffffffff);
      cap_data.data[0].inheritable = 0;
      cap_data.data[1].permitted = (uint32_t)(parsed.capabilities >> 32);
      cap_data.data[1].inheritable = 0;
      if (setxattr(filename, XATTR_NAME_CAPS, &cap_data, sizeof(cap_data), 0) < 0) {
        uiPrintf(state, "ApplyParsedPerms: setcap of %s to %" PRIx64 " failed: %s\n", filename,
                 parsed.capabilities, strerror(errno));
        bad++;
      }
    }
  }

  return bad;
}

// nftw doesn't allow us to pass along context, so we need to use
// global variables.  *sigh*
static struct perm_parsed_args recursive_parsed_args;
static State* recursive_state;

static int do_SetMetadataRecursive(const char* filename, const struct stat* statptr, int fileflags,
                                   struct FTW* pfwt) {
  return ApplyParsedPerms(recursive_state, filename, statptr, recursive_parsed_args);
}

static Value* SetMetadataFn(const char* name, State* state, const std::vector<std::unique_ptr<Expr>>& argv) {
  if ((argv.size() % 2) != 1) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects an odd number of arguments, got %zu",
                      name, argv.size());
  }

  std::vector<std::string> args;
  if (!ReadArgs(state, argv, &args)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse the argument(s)", name);
  }

  struct stat sb;
  if (lstat(args[0].c_str(), &sb) == -1) {
    return ErrorAbort(state, kSetMetadataFailure, "%s: Error on lstat of \"%s\": %s", name,
                      args[0].c_str(), strerror(errno));
  }

  struct perm_parsed_args parsed = ParsePermArgs(state, args);
  int bad = 0;
  bool recursive = (strcmp(name, "set_metadata_recursive") == 0);

  if (recursive) {
    recursive_parsed_args = parsed;
    recursive_state = state;
    bad += nftw(args[0].c_str(), do_SetMetadataRecursive, 30, FTW_CHDIR | FTW_DEPTH | FTW_PHYS);
    memset(&recursive_parsed_args, 0, sizeof(recursive_parsed_args));
    recursive_state = NULL;
  } else {
    bad += ApplyParsedPerms(state, args[0].c_str(), &sb, parsed);
  }

  if (bad > 0) {
    return ErrorAbort(state, kSetMetadataFailure, "%s: some changes failed", name);
  }

  return StringValue("");
}

Value* GetPropFn(const char* name, State* state, const std::vector<std::unique_ptr<Expr>>& argv) {
  if (argv.size() != 1) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects 1 arg, got %zu", name, argv.size());
  }
  std::string key;
  if (!Evaluate(state, argv[0], &key)) {
    return nullptr;
  }
  std::string value = android::base::GetProperty(key, "");

  return StringValue(value);
}

// file_getprop(file, key)
//
//   interprets 'file' as a getprop-style file (key=value pairs, one
//   per line. # comment lines, blank lines, lines without '=' ignored),
//   and returns the value for 'key' (or "" if it isn't defined).
Value* FileGetPropFn(const char* name, State* state,
                     const std::vector<std::unique_ptr<Expr>>& argv) {
  if (argv.size() != 2) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects 2 args, got %zu", name,
                      argv.size());
  }

  std::vector<std::string> args;
  if (!ReadArgs(state, argv, &args)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse the argument(s)", name);
  }
  const std::string& filename = args[0];
  const std::string& key = args[1];

  struct stat st;
  if (stat(filename.c_str(), &st) < 0) {
    return ErrorAbort(state, kFileGetPropFailure, "%s: failed to stat \"%s\": %s", name,
                      filename.c_str(), strerror(errno));
  }

  constexpr off_t MAX_FILE_GETPROP_SIZE = 65536;
  if (st.st_size > MAX_FILE_GETPROP_SIZE) {
    return ErrorAbort(state, kFileGetPropFailure, "%s too large for %s (max %lld)",
                      filename.c_str(), name, static_cast<long long>(MAX_FILE_GETPROP_SIZE));
  }

  std::string buffer(st.st_size, '\0');
  unique_file f(ota_fopen(filename.c_str(), "rb"));
  if (f == nullptr) {
    return ErrorAbort(state, kFileOpenFailure, "%s: failed to open %s: %s", name, filename.c_str(),
                      strerror(errno));
  }

  if (ota_fread(&buffer[0], 1, st.st_size, f.get()) != static_cast<size_t>(st.st_size)) {
    ErrorAbort(state, kFreadFailure, "%s: failed to read %zu bytes from %s", name,
               static_cast<size_t>(st.st_size), filename.c_str());
    return nullptr;
  }

  ota_fclose(f);

  std::vector<std::string> lines = android::base::Split(buffer, "\n");
  for (size_t i = 0; i < lines.size(); i++) {
    std::string line = android::base::Trim(lines[i]);

    // comment or blank line: skip to next line
    if (line.empty() || line[0] == '#') {
      continue;
    }
    size_t equal_pos = line.find('=');
    if (equal_pos == std::string::npos) {
      continue;
    }

    // trim whitespace between key and '='
    std::string str = android::base::Trim(line.substr(0, equal_pos));

    // not the key we're looking for
    if (key != str) continue;

    return StringValue(android::base::Trim(line.substr(equal_pos + 1)));
  }

  return StringValue("");
}

// apply_patch_space(bytes)
Value* ApplyPatchSpaceFn(const char* name, State* state,
                         const std::vector<std::unique_ptr<Expr>>& argv) {
  if (argv.size() != 1) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects 1 args, got %zu", name,
                      argv.size());
  }
  std::vector<std::string> args;
  if (!ReadArgs(state, argv, &args)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse the argument(s)", name);
  }
  const std::string& bytes_str = args[0];

  size_t bytes;
  if (!android::base::ParseUint(bytes_str.c_str(), &bytes)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s(): can't parse \"%s\" as byte count", name,
                      bytes_str.c_str());
  }

  // Skip the cache size check if the update is a retry.
  if (state->is_retry || CacheSizeCheck(bytes) == 0) {
    return StringValue("t");
  }
  return StringValue("");
}

Value* WipeCacheFn(const char* name, State* state, const std::vector<std::unique_ptr<Expr>>& argv) {
  if (!argv.empty()) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects no args, got %zu", name,
                      argv.size());
  }
  fprintf(static_cast<UpdaterInfo*>(state->cookie)->cmd_pipe, "wipe_cache\n");
  return StringValue("t");
}

Value* RunProgramFn(const char* name, State* state, const std::vector<std::unique_ptr<Expr>>& argv) {
  if (argv.size() < 1) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects at least 1 arg", name);
  }

  std::vector<std::string> args;
  if (!ReadArgs(state, argv, &args)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse the argument(s)", name);
  }

  char* args2[argv.size() + 1];
  for (size_t i = 0; i < argv.size(); i++) {
    args2[i] = &args[i][0];
  }
  args2[argv.size()] = nullptr;

  LOG(INFO) << "about to run program [" << args2[0] << "] with " << argv.size() << " args";

  pid_t child = fork();
  if (child == 0) {
    execv(args2[0], args2);
    PLOG(ERROR) << "run_program: execv failed";
    _exit(EXIT_FAILURE);
  }

  int status;
  waitpid(child, &status, 0);
  if (WIFEXITED(status)) {
    if (WEXITSTATUS(status) != 0) {
      LOG(ERROR) << "run_program: child exited with status " << WEXITSTATUS(status);
    }
  } else if (WIFSIGNALED(status)) {
    LOG(ERROR) << "run_program: child terminated by signal " << WTERMSIG(status);
  }

  return StringValue(std::to_string(status));
}

// Read a local file and return its contents (the Value* returned
// is actually a FileContents*).
Value* ReadFileFn(const char* name, State* state, const std::vector<std::unique_ptr<Expr>>& argv) {
  if (argv.size() != 1) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects 1 arg, got %zu", name, argv.size());
  }

  std::vector<std::string> args;
  if (!ReadArgs(state, argv, &args)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse the argument(s)", name);
  }
  const std::string& filename = args[0];

  Value* v = new Value(VAL_INVALID, "");

  FileContents fc;
  if (LoadFileContents(filename.c_str(), &fc) == 0) {
    v->type = VAL_BLOB;
    v->data = std::string(fc.data.begin(), fc.data.end());
  }
  return v;
}

// write_value(value, filename)
//   Writes 'value' to 'filename'.
//   Example: write_value("960000", "/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq")
Value* WriteValueFn(const char* name, State* state, const std::vector<std::unique_ptr<Expr>>& argv) {
  if (argv.size() != 2) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects 2 args, got %zu", name,
                      argv.size());
  }

  std::vector<std::string> args;
  if (!ReadArgs(state, argv, &args)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s(): Failed to parse the argument(s)", name);
  }

  const std::string& filename = args[1];
  if (filename.empty()) {
    return ErrorAbort(state, kArgsParsingFailure, "%s(): Filename cannot be empty", name);
  }

  const std::string& value = args[0];
  if (!android::base::WriteStringToFile(value, filename)) {
    PLOG(ERROR) << name << ": Failed to write to \"" << filename << "\"";
    return StringValue("");
  } else {
    return StringValue("t");
  }
}

// Immediately reboot the device.  Recovery is not finished normally,
// so if you reboot into recovery it will re-start applying the
// current package (because nothing has cleared the copy of the
// arguments stored in the BCB).
//
// The argument is the partition name passed to the android reboot
// property.  It can be "recovery" to boot from the recovery
// partition, or "" (empty string) to boot from the regular boot
// partition.
Value* RebootNowFn(const char* name, State* state, const std::vector<std::unique_ptr<Expr>>& argv) {
  if (argv.size() != 2) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects 2 args, got %zu", name,
                      argv.size());
  }

  std::vector<std::string> args;
  if (!ReadArgs(state, argv, &args)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s(): Failed to parse the argument(s)", name);
  }
  const std::string& filename = args[0];
  const std::string& property = args[1];

  // Zero out the 'command' field of the bootloader message. Leave the rest intact.
  bootloader_message boot;
  std::string err;
  if (!read_bootloader_message_from(&boot, filename, &err)) {
    LOG(ERROR) << name << "(): Failed to read from \"" << filename << "\": " << err;
    return StringValue("");
  }
  memset(boot.command, 0, sizeof(boot.command));
  if (!write_bootloader_message_to(boot, filename, &err)) {
    LOG(ERROR) << name << "(): Failed to write to \"" << filename << "\": " << err;
    return StringValue("");
  }

  std::string reboot_cmd = "reboot," + property;
  if (android::base::GetBoolProperty("ro.boot.quiescent", false)) {
    reboot_cmd += ",quiescent";
  }
  android::base::SetProperty(ANDROID_RB_PROPERTY, reboot_cmd);

  sleep(5);
  return ErrorAbort(state, kRebootFailure, "%s() failed to reboot", name);
}

// Store a string value somewhere that future invocations of recovery
// can access it.  This value is called the "stage" and can be used to
// drive packages that need to do reboots in the middle of
// installation and keep track of where they are in the multi-stage
// install.
//
// The first argument is the block device for the misc partition
// ("/misc" in the fstab), which is where this value is stored.  The
// second argument is the string to store; it should not exceed 31
// bytes.
Value* SetStageFn(const char* name, State* state, const std::vector<std::unique_ptr<Expr>>& argv) {
  if (argv.size() != 2) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects 2 args, got %zu", name,
                      argv.size());
  }

  std::vector<std::string> args;
  if (!ReadArgs(state, argv, &args)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse the argument(s)", name);
  }
  const std::string& filename = args[0];
  const std::string& stagestr = args[1];

  // Store this value in the misc partition, immediately after the
  // bootloader message that the main recovery uses to save its
  // arguments in case of the device restarting midway through
  // package installation.
  bootloader_message boot;
  std::string err;
  if (!read_bootloader_message_from(&boot, filename, &err)) {
    LOG(ERROR) << name << "(): Failed to read from \"" << filename << "\": " << err;
    return StringValue("");
  }
  strlcpy(boot.stage, stagestr.c_str(), sizeof(boot.stage));
  if (!write_bootloader_message_to(boot, filename, &err)) {
    LOG(ERROR) << name << "(): Failed to write to \"" << filename << "\": " << err;
    return StringValue("");
  }

  return StringValue(filename);
}

// Return the value most recently saved with SetStageFn.  The argument
// is the block device for the misc partition.
Value* GetStageFn(const char* name, State* state, const std::vector<std::unique_ptr<Expr>>& argv) {
  if (argv.size() != 1) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects 1 arg, got %zu", name, argv.size());
  }

  std::vector<std::string> args;
  if (!ReadArgs(state, argv, &args)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse the argument(s)", name);
  }
  const std::string& filename = args[0];

  bootloader_message boot;
  std::string err;
  if (!read_bootloader_message_from(&boot, filename, &err)) {
    LOG(ERROR) << name << "(): Failed to read from \"" << filename << "\": " << err;
    return StringValue("");
  }

  return StringValue(boot.stage);
}

Value* WipeBlockDeviceFn(const char* name, State* state, const std::vector<std::unique_ptr<Expr>>& argv) {
  if (argv.size() != 2) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects 2 args, got %zu", name,
                      argv.size());
  }

  std::vector<std::string> args;
  if (!ReadArgs(state, argv, &args)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() Failed to parse the argument(s)", name);
  }
  const std::string& filename = args[0];
  const std::string& len_str = args[1];

  size_t len;
  if (!android::base::ParseUint(len_str.c_str(), &len)) {
    return nullptr;
  }
  unique_fd fd(ota_open(filename.c_str(), O_WRONLY, 0644));
  // The wipe_block_device function in ext4_utils returns 0 on success and 1
  // for failure.
  int status = wipe_block_device(fd, len);
  return StringValue((status == 0) ? "t" : "");
}

Value* EnableRebootFn(const char* name, State* state, const std::vector<std::unique_ptr<Expr>>& argv) {
  if (!argv.empty()) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects no args, got %zu", name,
                      argv.size());
  }
  UpdaterInfo* ui = static_cast<UpdaterInfo*>(state->cookie);
  fprintf(ui->cmd_pipe, "enable_reboot\n");
  return StringValue("t");
}

Value* Tune2FsFn(const char* name, State* state, const std::vector<std::unique_ptr<Expr>>& argv) {
#ifdef HAVE_LIBTUNE2FS
  if (argv.empty()) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() expects args, got %zu", name, argv.size());
  }

  std::vector<std::string> args;
  if (!ReadArgs(state, argv, &args)) {
    return ErrorAbort(state, kArgsParsingFailure, "%s() could not read args", name);
  }

  char* args2[argv.size() + 1];
  // Tune2fs expects the program name as its args[0]
  args2[0] = const_cast<char*>(name);
  if (args2[0] == nullptr) {
    return nullptr;
  }
  for (size_t i = 0; i < argv.size(); ++i) {
    args2[i + 1] = &args[i][0];
  }

  // tune2fs changes the file system parameters on an ext2 file system; it
  // returns 0 on success.
  int result = tune2fs_main(argv.size() + 1, args2);
  if (result != 0) {
    return ErrorAbort(state, kTune2FsFailure, "%s() returned error code %d", name, result);
  }
  return StringValue("t");
#else
  return ErrorAbort(state, kTune2FsFailure, "%s() support not present, no libtune2fs", name);
#endif // HAVE_LIBTUNE2FS
}

void RegisterInstallFunctions() {
  RegisterFunction("mount", MountFn);
  RegisterFunction("is_mounted", IsMountedFn);
  RegisterFunction("unmount", UnmountFn);
  RegisterFunction("format", FormatFn);
  RegisterFunction("show_progress", ShowProgressFn);
  RegisterFunction("set_progress", SetProgressFn);
  RegisterFunction("delete", DeleteFn);
  RegisterFunction("delete_recursive", DeleteFn);
  RegisterFunction("package_extract_dir", PackageExtractDirFn);
  RegisterFunction("package_extract_file", PackageExtractFileFn);
  RegisterFunction("symlink", SymlinkFn);

  // Usage:
  //   set_metadata("filename", "key1", "value1", "key2", "value2", ...)
  // Example:
  //   set_metadata("/system/bin/netcfg", "uid", 0, "gid", 3003, "mode", 02750, "selabel",
  //                "u:object_r:system_file:s0", "capabilities", 0x0);
  RegisterFunction("set_metadata", SetMetadataFn);

  // Usage:
  //   set_metadata_recursive("dirname", "key1", "value1", "key2", "value2", ...)
  // Example:
  //   set_metadata_recursive("/system", "uid", 0, "gid", 0, "fmode", 0644, "dmode", 0755,
  //                          "selabel", "u:object_r:system_file:s0", "capabilities", 0x0);
  RegisterFunction("set_metadata_recursive", SetMetadataFn);

  RegisterFunction("getprop", GetPropFn);
  RegisterFunction("file_getprop", FileGetPropFn);

  RegisterFunction("apply_patch", ApplyPatchFn);
  RegisterFunction("apply_patch_check", ApplyPatchCheckFn);
  RegisterFunction("apply_patch_space", ApplyPatchSpaceFn);

  RegisterFunction("wipe_block_device", WipeBlockDeviceFn);

  RegisterFunction("read_file", ReadFileFn);
  RegisterFunction("sha1_check", Sha1CheckFn);
  RegisterFunction("rename", RenameFn);
  RegisterFunction("write_value", WriteValueFn);

  RegisterFunction("wipe_cache", WipeCacheFn);

  RegisterFunction("ui_print", UIPrintFn);

  RegisterFunction("run_program", RunProgramFn);

  RegisterFunction("reboot_now", RebootNowFn);
  RegisterFunction("get_stage", GetStageFn);
  RegisterFunction("set_stage", SetStageFn);

  RegisterFunction("enable_reboot", EnableRebootFn);
  RegisterFunction("tune2fs", Tune2FsFn);
}
