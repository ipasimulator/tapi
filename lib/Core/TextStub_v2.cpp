//===- tapi/Core/TextStub_v2.cpp - Text Stub v2 -----------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief Implements the text stub file (TBD v2) reader/writer.
///
//===----------------------------------------------------------------------===//

#include "tapi/Core/TextStub_v2.h"
#include "tapi/Core/ArchitectureSupport.h"
#include "tapi/Core/InterfaceFile.h"
#include "tapi/Core/Registry.h"
#include "tapi/Core/YAML.h"
#include "tapi/Core/YAMLReaderWriter.h"
#include "tapi/LinkerInterfaceFile.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/YAMLTraits.h"
#include <set>

using namespace llvm;
using namespace llvm::yaml;
using namespace TAPI_INTERNAL;

namespace {

struct ExportSection {
  std::vector<Architecture> archs;
  std::vector<FlowStringRef> allowableClients;
  std::vector<FlowStringRef> reexportedLibraries;
  std::vector<FlowStringRef> symbols;
  std::vector<FlowStringRef> classes;
  std::vector<FlowStringRef> ivars;
  std::vector<FlowStringRef> weakDefSymbols;
  std::vector<FlowStringRef> tlvSymbols;
};

struct UndefinedSection {
  std::vector<Architecture> archs;
  std::vector<FlowStringRef> symbols;
  std::vector<FlowStringRef> classes;
  std::vector<FlowStringRef> ivars;
  std::vector<FlowStringRef> weakRefSymbols;
};

enum Flags : unsigned {
  None                         = 0U,
  FlatNamespace                = 1U << 0,
  NotApplicationExtensionSafe  = 1U << 1,
  InstallAPI                   = 1U << 2,
};

inline Flags operator|(const Flags a, const Flags b) {
  return static_cast<Flags>(static_cast<unsigned>(a) |
                            static_cast<unsigned>(b));
}

inline Flags operator|=(Flags &a, const Flags b) {
  a = static_cast<Flags>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
  return a;
}

} // end anonymous namespace.

LLVM_YAML_IS_FLOW_SEQUENCE_VECTOR(Architecture)
LLVM_YAML_IS_SEQUENCE_VECTOR(ExportSection)
LLVM_YAML_IS_SEQUENCE_VECTOR(UndefinedSection)

namespace llvm {
namespace yaml {

template <> struct MappingTraits<ExportSection> {
  static void mapping(IO &io, ExportSection &section) {
    io.mapRequired("archs", section.archs);
    io.mapOptional("allowable-clients", section.allowableClients);
    io.mapOptional("re-exports", section.reexportedLibraries);
    io.mapOptional("symbols", section.symbols);
    io.mapOptional("objc-classes", section.classes);
    io.mapOptional("objc-ivars", section.ivars);
    io.mapOptional("weak-def-symbols", section.weakDefSymbols);
    io.mapOptional("thread-local-symbols", section.tlvSymbols);
  }
};

template <> struct MappingTraits<UndefinedSection> {
  static void mapping(IO &io, UndefinedSection &section) {
    io.mapRequired("archs", section.archs);
    io.mapOptional("symbols", section.symbols);
    io.mapOptional("objc-classes", section.classes);
    io.mapOptional("objc-ivars", section.ivars);
    io.mapOptional("weak-ref-symbols", section.weakRefSymbols);
  }
};

template <> struct ScalarBitSetTraits<Flags> {
  static void bitset(IO &io, Flags &flags) {
    io.bitSetCase(flags, "flat_namespace", Flags::FlatNamespace);
    io.bitSetCase(flags, "not_app_extension_safe",
                  Flags::NotApplicationExtensionSafe);
    io.bitSetCase(flags, "installapi", Flags::InstallAPI);
  }
};

template <> struct MappingTraits<const InterfaceFile *> {
  struct NormalizedTBD2 {
    explicit NormalizedTBD2(IO &io) {}
    NormalizedTBD2(IO &io, const InterfaceFile *&file) {
      archs = file->getArchitectures();
      uuids = file->uuids();
      platform = file->getPlatform();
      installName = file->getInstallName();
      currentVersion = file->getCurrentVersion();
      compatibilityVersion = file->getCompatibilityVersion();
      swiftVersion = file->getSwiftABIVersion();
      objcConstraint = file->getObjCConstraint();

      flags = Flags::None;
      if (!file->isApplicationExtensionSafe())
        flags |= Flags::NotApplicationExtensionSafe;

      if (!file->isTwoLevelNamespace())
        flags |= Flags::FlatNamespace;

      if (file->isInstallAPI())
        flags |= Flags::InstallAPI;

      parentUmbrella = file->getParentUmbrella();

      std::set<ArchitectureSet> archSet;
      for (const auto &library : file->allowableClients())
        archSet.insert(library.getArchitectures());

      for (const auto &library : file->reexportedLibraries())
        archSet.insert(library.getArchitectures());

      std::map<const Symbol *, ArchitectureSet> symbolToArchSet;
      for (const auto *symbol : file->exports()) {
        auto archs = symbol->getArchitectures();
        symbolToArchSet[symbol] = archs;
        archSet.insert(archs);
      }

      for (auto archs : archSet) {
        ExportSection section;
        section.archs = archs;

        for (const auto &library : file->allowableClients())
          if (library.getArchitectures() == archs)
            section.allowableClients.emplace_back(library.getInstallName());

        for (const auto &library : file->reexportedLibraries())
          if (library.getArchitectures() == archs)
            section.reexportedLibraries.emplace_back(library.getInstallName());

        for (const auto &symArch : symbolToArchSet) {
          if (symArch.second != archs)
            continue;

          const auto *symbol = symArch.first;
          switch (symbol->getKind()) {
          case SymbolKind::GlobalSymbol:
            if (symbol->isWeakDefined())
              section.weakDefSymbols.emplace_back(symbol->getName());
            else if (symbol->isThreadLocalValue())
              section.tlvSymbols.emplace_back(symbol->getName());
            else
              section.symbols.emplace_back(symbol->getName());
            break;
          case SymbolKind::ObjectiveCClass:
            section.classes.emplace_back(
                copyString("_" + symbol->getName().str()));
            break;
          case SymbolKind::ObjectiveCClassEHType:
            section.symbols.emplace_back(
                copyString("_OBJC_EHTYPE_$_" + symbol->getName().str()));
            break;
          case SymbolKind::ObjectiveCInstanceVariable:
            section.ivars.emplace_back(
                copyString("_" + symbol->getName().str()));
            break;
          }
        }

        // [port] CHANGED: See [sort].
#if defined(TAPI_PORT)
        using TAPI_INTERNAL::sort;
#endif

        sort(section.symbols);
        sort(section.classes);
        sort(section.ivars);
        sort(section.weakDefSymbols);
        sort(section.tlvSymbols);
        exports.emplace_back(std::move(section));
      }

      archSet.clear();
      symbolToArchSet.clear();

      for (const auto *symbol : file->undefineds()) {
        auto archs = symbol->getArchitectures();
        symbolToArchSet[symbol] = archs;
        archSet.insert(archs);
      }

      for (auto archs : archSet) {
        UndefinedSection section;
        section.archs = archs;

        for (const auto &symArch : symbolToArchSet) {
          if (symArch.second != archs)
            continue;

          const auto *symbol = symArch.first;
          switch (symbol->getKind()) {
          case SymbolKind::GlobalSymbol:
            if (symbol->isWeakReferenced())
              section.weakRefSymbols.emplace_back(symbol->getName());
            else
              section.symbols.emplace_back(symbol->getName());
            break;
          case SymbolKind::ObjectiveCClass:
            section.classes.emplace_back(
                copyString("_" + symbol->getName().str()));
            break;
          case SymbolKind::ObjectiveCClassEHType:
            section.symbols.emplace_back(
                copyString("_OBJC_EHTYPE_$_" + symbol->getName().str()));
            break;
          case SymbolKind::ObjectiveCInstanceVariable:
            section.ivars.emplace_back(
                copyString("_" + symbol->getName().str()));
            break;
          }
        }

        // [port] CHANGED: See [sort].
#if defined(TAPI_PORT)
        using TAPI_INTERNAL::sort;
#endif

        sort(section.symbols);
        sort(section.classes);
        sort(section.ivars);
        sort(section.weakRefSymbols);
        undefineds.emplace_back(std::move(section));
      }
    }

    const InterfaceFile *denormalize(IO &io) {
      auto ctx = reinterpret_cast<YAMLContext *>(io.getContext());
      assert(ctx);

      auto *file = new InterfaceFile;
      file->setPath(ctx->path);
      file->setFileType(TAPI_INTERNAL::FileType::TBD_V2);
      for (auto &id : uuids)
        file->addUUID(id.first, id.second);
      file->setPlatform(platform);
      file->setArchitectures(archs);
      file->setInstallName(installName);
      file->setCurrentVersion(currentVersion);
      file->setCompatibilityVersion(compatibilityVersion);
      file->setSwiftABIVersion(swiftVersion);
      file->setObjCConstraint(objcConstraint);
      file->setParentUmbrella(parentUmbrella);

      file->setTwoLevelNamespace(!(flags & Flags::FlatNamespace));
      file->setApplicationExtensionSafe(
          !(flags & Flags::NotApplicationExtensionSafe));
      file->setInstallAPI(flags & Flags::InstallAPI);

      for (const auto &section : exports) {
        for (const auto &client : section.allowableClients)
          file->addAllowableClient(client, section.archs);
        for (const auto &lib : section.reexportedLibraries)
          file->addReexportedLibrary(lib, section.archs);

        // Skip symbols if requested.
        if (ctx->readFlags < ReadFlags::Symbols)
          continue;

        for (auto &sym : section.symbols) {
          if (sym.value.startswith("_OBJC_EHTYPE_$_"))
            file->addSymbolImpl(SymbolKind::ObjectiveCClassEHType,
                                sym.value.drop_front(15), section.archs,
                                SymbolFlags::None, /*copyStrings=*/false);
          else
            file->addSymbolImpl(SymbolKind::GlobalSymbol, sym, section.archs,
                                SymbolFlags::None,
                                /*copyStrings=*/false);
        }
        for (auto &sym : section.classes)
          file->addSymbolImpl(SymbolKind::ObjectiveCClass,
                              sym.value.drop_front(), section.archs,
                              SymbolFlags::None, /*copyStrings=*/false);
        for (auto &sym : section.ivars)
          file->addSymbolImpl(SymbolKind::ObjectiveCInstanceVariable,
                              sym.value.drop_front(), section.archs,
                              SymbolFlags::None, /*copyStrings=*/false);
        for (auto &sym : section.weakDefSymbols)
          file->addSymbolImpl(SymbolKind::GlobalSymbol, sym, section.archs,
                              SymbolFlags::WeakDefined,
                              /*copyStrings=*/false);
        for (auto &sym : section.tlvSymbols)
          file->addSymbolImpl(SymbolKind::GlobalSymbol, sym, section.archs,
                              SymbolFlags::ThreadLocalValue,
                              /*copyStrings=*/false);
      }

      // Skip symbols if requested.
      if (ctx->readFlags < ReadFlags::Symbols)
        return file;

      for (const auto &section : undefineds) {
        for (auto &sym : section.symbols) {
          if (sym.value.startswith("_OBJC_EHTYPE_$_"))
            file->addUndefinedSymbolImpl(SymbolKind::ObjectiveCClassEHType,
                                         sym.value.drop_front(15),
                                         section.archs, SymbolFlags::None,
                                         /*copyStrings=*/false);
          else
            file->addUndefinedSymbolImpl(SymbolKind::GlobalSymbol, sym,
                                         section.archs, SymbolFlags::None,
                                         /*copyStrings=*/false);
        }
        for (auto &sym : section.classes)
          file->addUndefinedSymbolImpl(SymbolKind::ObjectiveCClass,
                                       sym.value.drop_front(), section.archs,
                                       SymbolFlags::None,
                                       /*copyStrings=*/false);
        for (auto &sym : section.ivars)
          file->addUndefinedSymbolImpl(SymbolKind::ObjectiveCInstanceVariable,
                                       sym.value.drop_front(), section.archs,
                                       SymbolFlags::None,
                                       /*copyStrings=*/false);
        for (auto &sym : section.weakRefSymbols)
          file->addUndefinedSymbolImpl(SymbolKind::GlobalSymbol, sym,
                                       section.archs,
                                       SymbolFlags::WeakReferenced,
                                       /*copyStrings=*/false);
      }

      return file;
    }

    std::vector<Architecture> archs;
    std::vector<UUID> uuids;
    Platform platform;
    StringRef installName;
    PackedVersion currentVersion;
    PackedVersion compatibilityVersion;
    SwiftVersion swiftVersion;
    ObjCConstraint objcConstraint;
    Flags flags;
    StringRef parentUmbrella;
    std::vector<ExportSection> exports;
    std::vector<UndefinedSection> undefineds;

    llvm::BumpPtrAllocator allocator;
    StringRef copyString(StringRef string) {
      if (string.empty())
        return {};

      void *ptr = allocator.Allocate(string.size(), 1);
      memcpy(ptr, string.data(), string.size());
      return {reinterpret_cast<const char *>(ptr), string.size()};
    }
  };

  static void mappingTBD2(IO &io, const InterfaceFile *&file) {
    MappingNormalization<NormalizedTBD2, const InterfaceFile *> keys(io, file);
    io.mapTag("!tapi-tbd-v2", true);
    io.mapRequired("archs", keys->archs);
    io.mapOptional("uuids", keys->uuids);
    io.mapRequired("platform", keys->platform);
    io.mapOptional("flags", keys->flags, Flags::None);
    io.mapRequired("install-name", keys->installName);
    io.mapOptional("current-version", keys->currentVersion,
                   PackedVersion(1, 0, 0));
    io.mapOptional("compatibility-version", keys->compatibilityVersion,
                   PackedVersion(1, 0, 0));
    io.mapOptional("swift-version", keys->swiftVersion, SwiftVersion(0));
    io.mapOptional("objc-constraint", keys->objcConstraint,
                   ObjCConstraint::Retain_Release);
    io.mapOptional("parent-umbrella", keys->parentUmbrella, StringRef());
    io.mapOptional("exports", keys->exports);
    io.mapOptional("undefineds", keys->undefineds);
  }
};

} // end namespace yaml.
} // end namespace llvm.

TAPI_NAMESPACE_INTERNAL_BEGIN

namespace stub {
namespace v2 {

bool YAMLDocumentHandler::canRead(MemoryBufferRef memBufferRef,
                                  FileType types) const {
  if (!(types & FileType::TBD_V2))
    return false;

  auto str = memBufferRef.getBuffer().trim();
  if (!str.startswith("--- !tapi-tbd-v2\n") || !str.endswith("..."))
    return false;

  return true;
}

FileType YAMLDocumentHandler::getFileType(MemoryBufferRef memBufferRef) const {
  if (canRead(memBufferRef))
    return FileType::TBD_V2;

  return FileType::Invalid;
}

bool YAMLDocumentHandler::canWrite(const File *file) const {
  auto *interface = dyn_cast<InterfaceFile>(file);
  if (interface == nullptr)
    return false;

  if (interface->getFileType() != FileType::TBD_V2)
    return false;

  return true;
}

bool YAMLDocumentHandler::handleDocument(IO &io, const File *&file) const {
  if (io.outputting() && file->getFileType() != FileType::TBD_V2)
    return false;

  if (!io.outputting() && !io.mapTag("!tapi-tbd-v2"))
    return false;

  const auto *interface = dyn_cast_or_null<InterfaceFile>(file);
  MappingTraits<const InterfaceFile *>::mappingTBD2(io, interface);
  file = interface;

  return true;
}

} // end namespace v2.
} // end namespace stub.

TAPI_NAMESPACE_INTERNAL_END
