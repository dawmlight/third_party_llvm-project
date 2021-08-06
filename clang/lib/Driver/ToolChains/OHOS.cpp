//===--- OHOS.cpp - OHOS ToolChain Implementations --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// 2021.7.19 Add OHOS target support for clang
//     Copyright (c) 2021 Huawei Device Co., Ltd. All rights reserved.

#include "OHOS.h"
#include "Arch/ARM.h"
#include "CommonArgs.h"
#include "clang/Config/config.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/DriverDiagnostic.h"
#include "clang/Driver/Options.h"
#include "clang/Driver/SanitizerArgs.h"
#include "llvm/Option/ArgList.h"
#include "llvm/ProfileData/InstrProf.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/ScopedPrinter.h"

using namespace clang::driver;
using namespace clang::driver::toolchains;
using namespace clang::driver::tools;
using namespace clang;
using namespace llvm::opt;
using namespace clang::driver::tools::arm;

using tools::addMultilibFlag;
using tools::addPathIfExists;

// Fix build with --enable-assertions errors.
// Use this function to replace arm::getARMFloatABI on OHOS Constructor.
// Because on OHOS Constructor stage the TC.getEffectiveTriple is
// not initialized, call arm::getARMFloatABI will cause assert.
// Select the float ABI as determined by -msoft-float, -mhard-float, and
// -mfloat-abi=.
static arm::FloatABI getARMFloatABI(const Driver &D,
                                    const llvm::Triple &Triple,
                                    const ArgList &Args) {
  arm::FloatABI ABI = FloatABI::Soft;
  if (Arg *A =
          Args.getLastArg(options::OPT_msoft_float, options::OPT_mhard_float,
                          options::OPT_mfloat_abi_EQ)) {
    if (A->getOption().matches(options::OPT_msoft_float)) {
      ABI = FloatABI::Soft;
    } else if (A->getOption().matches(options::OPT_mhard_float)) {
      ABI = FloatABI::Hard;
    } else {
      ABI = llvm::StringSwitch<arm::FloatABI>(A->getValue())
                .Case("soft", FloatABI::Soft)
                .Case("softfp", FloatABI::SoftFP)
                .Case("hard", FloatABI::Hard)
                .Default(FloatABI::Invalid);
      if (ABI == FloatABI::Invalid && !StringRef(A->getValue()).empty()) {
        D.Diag(diag::err_drv_invalid_mfloat_abi) << A->getAsString(Args);
        ABI = FloatABI::Soft;
      }
    }
  }
  return ABI;
}

static bool findOHOSMuslMultilibs(const Multilib::flags_list &Flags,
                                  DetectedMultilibs &Result) {
  MultilibSet Multilibs;
  Multilibs.push_back(Multilib());
  // -mcpu=cortex-a7
  // -mfloat-abi=soft -mfloat-abi=softfp -mfloat-abi=hard
  // -mfpu=neon-vfpv4
  Multilibs.push_back(Multilib("a7_soft", {}, {}, 1)
                          .flag("+mcpu=cortex-a7")
                          .flag("+mfloat-abi=soft"));

  Multilibs.push_back(Multilib("a7_softfp_neon-vfpv4", {}, {}, 1)
                          .flag("+mcpu=cortex-a7")
                          .flag("+mfloat-abi=softfp")
                          .flag("+mfpu=neon-vfpv4"));

  Multilibs.push_back(Multilib("a7_hard_neon-vfpv4", {}, {}, 1)
                          .flag("+mcpu=cortex-a7")
                          .flag("+mfloat-abi=hard")
                          .flag("+mfpu=neon-vfpv4"));

  if (Multilibs.select(Flags, Result.SelectedMultilib)) {
    Result.Multilibs = Multilibs;
    return true;
  }
  return false;
}

static bool findOHOSMultilibs(const Driver &D,
                                      const ToolChain &TC,
                                      const llvm::Triple &TargetTriple,
                                      StringRef Path, const ArgList &Args,
                                      DetectedMultilibs &Result) {
  Multilib::flags_list Flags;
  bool IsA7 = false;
  if (const Arg *A = Args.getLastArg(options::OPT_mcpu_EQ))
    IsA7 = A->getValue() == StringRef("cortex-a7");
  addMultilibFlag(IsA7, "mcpu=cortex-a7", Flags);

  bool IsMFPU = false;
  if (const Arg *A = Args.getLastArg(options::OPT_mfpu_EQ))
    IsMFPU = A->getValue() == StringRef("neon-vfpv4");
  addMultilibFlag(IsMFPU, "mfpu=neon-vfpv4", Flags);

  tools::arm::FloatABI ARMFloatABI = getARMFloatABI(D, TargetTriple, Args);
  addMultilibFlag((ARMFloatABI == tools::arm::FloatABI::Soft),
      "mfloat-abi=soft", Flags);
  addMultilibFlag((ARMFloatABI == tools::arm::FloatABI::SoftFP),
      "mfloat-abi=softfp", Flags);
  addMultilibFlag((ARMFloatABI == tools::arm::FloatABI::Hard),
      "mfloat-abi=hard", Flags);

  return findOHOSMuslMultilibs(Flags, Result);
}

std::string OHOS::getMultiarchTriple(const Driver &D,
                                     const llvm::Triple &TargetTriple,
                                     StringRef SysRoot) const {
  bool IsLiteOS = TargetTriple.isOSLiteOS();
  const bool IsOHOSMusl = TargetTriple.isOHOSMusl();
  auto GetTargetWithPostfix = IsOHOSMusl ?
            [](const char* t){ return std::string(t) + "musl"; } :
            [](const char* t){ return std::string(t); };
  // For most architectures, just use whatever we have rather than trying to be
  // clever.
  switch (TargetTriple.getArch()) {
  default:
    break;

  // We use the existence of '/lib/<triple>' as a directory to detect some
  // common linux triples that don't quite match the Clang triple for both
  // 32-bit and 64-bit targets. Multiarch fixes its install triples to these
  // regardless of what the actual target triple is.
  case llvm::Triple::arm:
  case llvm::Triple::thumb:
    if (IsLiteOS) {
      return "arm-liteos";
    } else {
      return GetTargetWithPostfix("arm-linux-ohos");
    }
  case llvm::Triple::riscv32:
    if (IsLiteOS) {
      return "riscv32-liteos";
    } else {
      return "riscv32-linux-ohos";
    }
  case llvm::Triple::x86:
    return "i686-linux-ohos";
  case llvm::Triple::x86_64:
    return "x86_64-linux-ohos";
  case llvm::Triple::aarch64:
    return GetTargetWithPostfix("aarch64-linux-ohos");
  }
  return TargetTriple.str();
}

/// OHOS Toolchain
OHOS::OHOS(const Driver &D, const llvm::Triple &Triple, const ArgList &Args)
    : Generic_ELF(D, Triple, Args) {
  std::string SysRoot = computeSysRoot();

  // Select the correct multilib according to the given arguments.
  DetectedMultilibs Result;
  findOHOSMultilibs(D, *this, Triple, "", Args, Result);
  Multilibs = Result.Multilibs;
  SelectedMultilib = Result.SelectedMultilib;

  // FIXME: We have to (almost) duplicate logic in Toolchain::Toolchain,
  // because the latter is called when OHOS virtual methods are not yet
  // accessible
  getFilePaths().clear();
  if (D.CCCIsCXX()) {
    if (auto CXXStdlibPath = getCXXStdlibPath())
      getFilePaths().push_back(*CXXStdlibPath);
  }

  // FIXME: Make Toolchain::getArchSpecificLibPath virtual. We'll still have
  // to duplicate code here, but at least we'll no longer need `if (OHOS)`
  // style hacks in base classes.
  std::string CandidateLibPath = getArchSpecificLibPath();
  if (getVFS().exists(CandidateLibPath))
    getFilePaths().push_back(CandidateLibPath);

  getLibraryPaths().clear();
  if (auto RuntimePath = getRuntimePath())
    getLibraryPaths().push_back(*RuntimePath);

  // OHOS sysroots contain a library directory for each supported OS
  // version as well as some unversioned libraries in the usual multiarch
  // directory. Support --target=aarch64-linux-ohosX.Y.Z or
  // --target=aarch64-linux-ohosX.Y or --target=aarch64-linux-ohosX
  unsigned Major;
  unsigned Minor;
  unsigned Micro;
  Triple.getEnvironmentVersion(Major, Minor, Micro);
  const std::string MultiarchTriple = getMultiarchTriple(D, Triple, SysRoot);
  path_list &Paths = getFilePaths();
  addPathIfExists(D,
        SysRoot + "/usr/lib/" + MultiarchTriple + "/" +
        llvm::to_string(Major) + "." + llvm::to_string(Minor) +
        "." + llvm::to_string(Micro) + SelectedMultilib.gccSuffix(), Paths);
  addPathIfExists(D,
        SysRoot + "/usr/lib/" + MultiarchTriple + "/" +
        llvm::to_string(Major) + "." + llvm::to_string(Minor) +
        SelectedMultilib.gccSuffix(), Paths);
  addPathIfExists(D,
        SysRoot + "/usr/lib/" + MultiarchTriple + "/" +
        llvm::to_string(Major) + SelectedMultilib.gccSuffix(), Paths);

  addPathIfExists(D, SysRoot + "/usr/lib/" + MultiarchTriple +
        SelectedMultilib.gccSuffix(), Paths);
}

std::string OHOS::ComputeEffectiveClangTriple(const ArgList &Args,
                                                 types::ID InputType) const {
  // Don't modify this, it is impact of the toolchain and target init process.
  return ComputeLLVMTriple(Args, InputType);
}

ToolChain::RuntimeLibType OHOS::GetRuntimeLibType(
    const ArgList &Args) const {
  if (Arg *A = Args.getLastArg(clang::driver::options::OPT_rtlib_EQ)) {
    StringRef Value = A->getValue();
    if (Value != "compiler-rt")
      getDriver().Diag(clang::diag::err_drv_invalid_rtlib_name)
          << A->getAsString(Args);
  }

  return ToolChain::RLT_CompilerRT;
}

ToolChain::CXXStdlibType
OHOS::GetCXXStdlibType(const ArgList &Args) const {
  if (Arg *A = Args.getLastArg(options::OPT_stdlib_EQ)) {
    StringRef Value = A->getValue();
    if (Value != "libc++")
      getDriver().Diag(diag::err_drv_invalid_stdlib_name)
        << A->getAsString(Args);
  }

  return ToolChain::CST_Libcxx;
}

void OHOS::addClangTargetOptions(const ArgList &DriverArgs,
                                    ArgStringList &CC1Args,
                                    Action::OffloadKind) const {
  if (DriverArgs.hasFlag(options::OPT_fuse_init_array,
                         options::OPT_fno_use_init_array, true))
    CC1Args.push_back("-fuse-init-array");
}

void OHOS::AddClangSystemIncludeArgs(const ArgList &DriverArgs,
                                        ArgStringList &CC1Args) const {
  const Driver &D = getDriver();
  const llvm::Triple &Triple = getTriple();
  std::string SysRoot = computeSysRoot();

  if (DriverArgs.hasArg(options::OPT_nostdinc))
    return;

  if (!DriverArgs.hasArg(options::OPT_nobuiltininc)) {
    SmallString<128> P(D.ResourceDir);
    llvm::sys::path::append(P, "include");
    addSystemInclude(DriverArgs, CC1Args, P);
  }

  if (DriverArgs.hasArg(options::OPT_nostdlibinc))
    return;

  // Check for configure-time C include directories.
  StringRef CIncludeDirs(C_INCLUDE_DIRS);
  if (CIncludeDirs != "") {
    SmallVector<StringRef, 5> dirs;
    CIncludeDirs.split(dirs, ":");
    for (StringRef dir : dirs) {
      StringRef Prefix =
          llvm::sys::path::is_absolute(dir) ? StringRef(SysRoot) : "";
      addExternCSystemInclude(DriverArgs, CC1Args, Prefix + dir);
    }
    return;
  }

  addExternCSystemInclude(DriverArgs, CC1Args, SysRoot + "/usr/include/" +
                          getMultiarchTripleForPath(Triple));
  addExternCSystemInclude(DriverArgs, CC1Args, SysRoot + "/include");
  addExternCSystemInclude(DriverArgs, CC1Args, SysRoot + "/usr/include");
}

void OHOS::AddClangCXXStdlibIncludeArgs(const ArgList &DriverArgs,
                                           ArgStringList &CC1Args) const {
  if (DriverArgs.hasArg(options::OPT_nostdlibinc) ||
      DriverArgs.hasArg(options::OPT_nostdincxx))
    return;

  switch (GetCXXStdlibType(DriverArgs)) {
  case ToolChain::CST_Libcxx: {
    SmallString<128> P(getDriver().Dir);
    llvm::sys::path::append(P, "..", "include", "c++", "v1");
    addSystemInclude(DriverArgs, CC1Args, P.str());
    break;
  }

  default:
    llvm_unreachable("invalid stdlib name");
  }
}

void OHOS::AddCXXStdlibLibArgs(const ArgList &Args,
                                  ArgStringList &CmdArgs) const {
  switch (GetCXXStdlibType(Args)) {
  case ToolChain::CST_Libcxx:
    CmdArgs.push_back("-lc++");
    CmdArgs.push_back("-lc++abi");
    CmdArgs.push_back("-lunwind");
    break;

  case ToolChain::CST_Libstdcxx:
    llvm_unreachable("invalid stdlib name");
  }
}

std::string OHOS::computeSysRoot() const {
  if (!getDriver().SysRoot.empty())
    return getDriver().SysRoot;

  const std::string InstalledDir(getDriver().getInstalledDir());
  std::string SysRootPath =
      InstalledDir + "/../../sysroot";
  if (llvm::sys::fs::exists(SysRootPath))
    return SysRootPath;

  return std::string();
}

Optional<std::string> OHOS::getRuntimePath() const {
  SmallString<128> P;
  const Driver &D = getDriver();
  const llvm::Triple &Triple = getTriple();

  // First try the triple passed to driver as --target=<triple>.
  P.assign(D.ResourceDir);
  llvm::sys::path::append(P, "lib", D.getTargetTriple(), SelectedMultilib.gccSuffix());
  if (getVFS().exists(P))
    return llvm::Optional<std::string>(P.str());

  // Second try the normalized triple.
  P.assign(D.ResourceDir);
  llvm::sys::path::append(P, "lib", Triple.str(), SelectedMultilib.gccSuffix());
  if (getVFS().exists(P))
    return llvm::Optional<std::string>(P.str());

  // Third try the effective triple.
  P.assign(D.ResourceDir);
  llvm::sys::path::append(P, "lib", getMultiarchTripleForPath(Triple), SelectedMultilib.gccSuffix());
  if (getVFS().exists(P))
    return llvm::Optional<std::string>(P.str());

  return None;
}

Optional<std::string> OHOS::getCXXStdlibPath() const {
  SmallString<128> P;
  const Driver &D = getDriver();
  const llvm::Triple &Triple = getTriple();

  // First try the triple passed to driver as --target=<triple>.
  P.assign(D.Dir);
  llvm::sys::path::append(P, "../lib", D.getTargetTriple(), "c++", SelectedMultilib.gccSuffix());
  if (getVFS().exists(P))
    return llvm::Optional<std::string>(P.str());

  // Second try the normalized triple.
  P.assign(D.Dir);
  llvm::sys::path::append(P, "../lib", Triple.str(), "c++", SelectedMultilib.gccSuffix());
  if (getVFS().exists(P))
    return llvm::Optional<std::string>(P.str());

  // Third try the effective triple.
  P.assign(D.Dir);
  llvm::sys::path::append(P, "../lib", getMultiarchTripleForPath(Triple), "c++", SelectedMultilib.gccSuffix());
  if (getVFS().exists(P))
    return llvm::Optional<std::string>(P.str());

  return None;
}

std::string OHOS::getDynamicLinker(const ArgList &Args) const {
  const llvm::Triple &Triple = getTriple();
  const llvm::Triple::ArchType Arch = getArch();

  // OHOS dynamic linker rename later, depend on the system design of OHOS.
  if (Triple.isOHOSBionic())
    return Triple.isArch64Bit() ? "/system/bin/linker64" : "/system/bin/linker";

  assert(Triple.isMusl());
  std::string ArchName;
  bool IsArm = false;

  switch (Arch) {
  case llvm::Triple::arm:
  case llvm::Triple::thumb:
    ArchName = "arm";
    IsArm = true;
    break;
  case llvm::Triple::armeb:
  case llvm::Triple::thumbeb:
    ArchName = "armeb";
    IsArm = true;
    break;
  default:
    ArchName = Triple.getArchName().str();
  }
  if (IsArm &&
      (tools::arm::getARMFloatABI(*this, Args) == tools::arm::FloatABI::Hard))
    ArchName += "hf";

  return "/lib/ld-musl-" + ArchName + ".so.1";
}

std::string OHOS::getCompilerRT(const ArgList &Args,
                                             StringRef Component,
                                             FileType Type) const {
  SmallString<128> Path(getDriver().ResourceDir);
  llvm::sys::path::append(Path, "lib", getDriver().getTargetTriple(), SelectedMultilib.gccSuffix());
  const char *Prefix =
      Type == ToolChain::FT_Object ? "" : "lib";
  const char *Suffix;
  switch (Type) {
  case ToolChain::FT_Object:
    Suffix = ".o";
    break;
  case ToolChain::FT_Static:
    Suffix = ".a";
    break;
  case ToolChain::FT_Shared:
    Suffix = ".so";
    break;
  }
  llvm::sys::path::append(
      Path, Prefix + Twine("clang_rt.") + Component + Suffix);
  return Path.str();
}

void OHOS::addExtraOpts(llvm::opt::ArgStringList &CmdArgs) const {
  CmdArgs.push_back("-z");
  CmdArgs.push_back("now");
  CmdArgs.push_back("-z");
  CmdArgs.push_back("relro");
  CmdArgs.push_back("-z");
  CmdArgs.push_back("max-page-size=4096");
  CmdArgs.push_back("--hash-style=gnu");
  // FIXME: gnu or both???
  CmdArgs.push_back("--hash-style=both");
#ifdef ENABLE_LINKER_BUILD_ID
  CmdArgs.push_back("--build-id");
#endif
  CmdArgs.push_back("--enable-new-dtags");
}

SanitizerMask OHOS::getSupportedSanitizers() const {
  SanitizerMask Res = ToolChain::getSupportedSanitizers();
  Res |= SanitizerKind::Address;
  Res |= SanitizerKind::PointerCompare;
  Res |= SanitizerKind::PointerSubtract;
  Res |= SanitizerKind::Fuzzer;
  Res |= SanitizerKind::FuzzerNoLink;
  Res |= SanitizerKind::Memory;
  Res |= SanitizerKind::Vptr;
  Res |= SanitizerKind::SafeStack;
  Res |= SanitizerKind::Scudo;
  // TODO: kASAN for liteos ??
  // TODO: Support TSAN and HWASAN and update mask.
  return Res;
}

// TODO: Make a base class for Linux and OHOS and move this there.
void OHOS::addProfileRTLibs(const llvm::opt::ArgList &Args,
                             llvm::opt::ArgStringList &CmdArgs) const {
  if (!needsProfileRT(Args)) return;

  // Add linker option -u__llvm_runtime_variable to cause runtime
  // initialization module to be linked in.
  if ((!Args.hasArg(options::OPT_coverage)) &&
      (!Args.hasArg(options::OPT_ftest_coverage)))
    CmdArgs.push_back(Args.MakeArgString(
        Twine("-u", llvm::getInstrProfRuntimeHookVarName())));
  ToolChain::addProfileRTLibs(Args, CmdArgs);
}
