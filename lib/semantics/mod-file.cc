// Copyright (c) 2018, NVIDIA CORPORATION.  All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "mod-file.h"
#include "scope.h"
#include "symbol.h"
#include "../parser/message.h"
#include "../parser/parsing.h"
#include <algorithm>
#include <cerrno>
#include <fstream>
#include <functional>
#include <ostream>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace Fortran::semantics {

using namespace parser::literals;

// The extension used for module files.
static constexpr auto extension{".mod"};
// The initial characters of a file that identify it as a .mod file.
static constexpr auto magic{"!mod$"};

// Helpers for creating error messages.
static parser::Message Error(
    const SourceName &, parser::MessageFixedText, const std::string &);
static parser::Message Error(const SourceName &, parser::MessageFixedText,
    const std::string &, const std::string &);

static const SourceName *GetSubmoduleParent(const parser::Program &);
static std::string ModFilePath(
    const std::string &, const SourceName &, const std::string &);
static void PutEntity(std::ostream &, const Symbol &);
static void PutObjectEntity(std::ostream &, const Symbol &);
static void PutProcEntity(std::ostream &, const Symbol &);
static void PutEntity(std::ostream &, const Symbol &, std::function<void()>);
static std::ostream &PutAttrs(
    std::ostream &, Attrs, std::string before = ","s, std::string after = ""s);
static std::ostream &PutLower(std::ostream &, const Symbol &);
static std::ostream &PutLower(std::ostream &, const DeclTypeSpec &);
static std::ostream &PutLower(std::ostream &, const std::string &);
static std::string CheckSum(const std::string &);
static bool MakeReadonly(const std::string &);

bool ModFileWriter::WriteAll() {
  WriteChildren(Scope::globalScope);
  return errors_.empty();
}

void ModFileWriter::WriteChildren(const Scope &scope) {
  for (const auto &child : scope.children()) {
    WriteOne(child);
  }
}

void ModFileWriter::WriteOne(const Scope &scope) {
  if (scope.kind() == Scope::Kind::Module) {
    auto *symbol{scope.symbol()};
    if (!symbol->test(Symbol::Flag::ModFile)) {
      Write(*symbol);
    }
    WriteChildren(scope);  // write out submodules
  }
}

// Write the module file for symbol, which must be a module or submodule.
void ModFileWriter::Write(const Symbol &symbol) {
  auto *ancestor{symbol.get<ModuleDetails>().ancestor()};
  auto ancestorName{ancestor ? ancestor->name().ToString() : ""s};
  auto path{ModFilePath(dir_, symbol.name(), ancestorName)};
  unlink(path.data());
  std::ofstream os{path};
  PutSymbols(*symbol.scope());
  std::string all{GetAsString(symbol)};
  auto header{GetHeader(all)};
  os << header << all;
  os.close();
  if (!os) {
    errors_.emplace_back(
        "Error writing %s: %s"_err_en_US, path.c_str(), std::strerror(errno));
    return;
  }
  if (!MakeReadonly(path)) {
    errors_.emplace_back("Error changing permissions on %s: %s"_en_US,
        path.c_str(), std::strerror(errno));
  }
}

// Return the entire body of the module file
// and clear saved uses, decls, and contains.
std::string ModFileWriter::GetAsString(const Symbol &symbol) {
  std::stringstream all;
  auto &details{symbol.get<ModuleDetails>()};
  if (!details.isSubmodule()) {
    PutLower(all << "module ", symbol);
  } else {
    auto *parent{details.parent()->symbol()};
    auto *ancestor{details.ancestor()->symbol()};
    PutLower(all << "submodule(", *ancestor);
    if (parent != ancestor) {
      PutLower(all << ':', *parent);
    }
    PutLower(all << ") ", symbol);
  }
  all << '\n' << uses_.str();
  uses_.str(""s);
  all << useExtraAttrs_.str();
  useExtraAttrs_.str(""s);
  all << decls_.str();
  decls_.str(""s);
  auto str{contains_.str()};
  contains_.str(""s);
  if (!str.empty()) {
    all << "contains\n" << str;
  }
  all << "end\n";
  return all.str();
}

// Return the header for this mod file.
std::string ModFileWriter::GetHeader(const std::string &all) {
  std::stringstream ss;
  ss << magic << " v" << version_ << " sum:" << CheckSum(all) << '\n';
  return ss.str();
}

// Put out the visible symbols from scope.
void ModFileWriter::PutSymbols(const Scope &scope) {
  for (const auto *symbol : SortSymbols(CollectSymbols(scope))) {
    PutSymbol(*symbol);
  }
}

// Sort symbols by their original order, not by name.
ModFileWriter::symbolVector ModFileWriter::SortSymbols(
    const ModFileWriter::symbolSet symbols) {
  ModFileWriter::symbolVector sorted;
  sorted.reserve(symbols.size());
  for (const auto *symbol : symbols) {
    sorted.push_back(symbol);
  }
  auto compare{[](const Symbol *x, const Symbol *y) {
    return x->name().begin() < y->name().begin();
  }};
  std::sort(sorted.begin(), sorted.end(), compare);
  return sorted;
}

// Return all symbols needed from this scope.
ModFileWriter::symbolSet ModFileWriter::CollectSymbols(const Scope &scope) {
  ModFileWriter::symbolSet symbols;
  for (const auto &pair : scope) {
    auto *symbol{pair.second};
    // include all components of derived types and other non-private symbols
    if (scope.kind() == Scope::Kind::DerivedType ||
        !symbol->attrs().test(Attr::PRIVATE)) {
      symbols.insert(symbol);
      // ensure the type symbol is included too, even if private
      if (const auto *type{symbol->GetType()}) {
        auto category{type->category()};
        if (category == DeclTypeSpec::TypeDerived ||
            category == DeclTypeSpec::ClassDerived) {
          auto *typeSymbol{type->derivedTypeSpec().scope()->symbol()};
          symbols.insert(typeSymbol);
        }
      }
      // TODO: other related symbols, e.g. in initial values
    }
  }
  return symbols;
}

void ModFileWriter::PutSymbol(const Symbol &symbol) {
  std::visit(
      common::visitors{
          [&](const ModuleDetails &) { /* should be current module */ },
          [&](const DerivedTypeDetails &) { PutDerivedType(symbol); },
          [&](const SubprogramDetails &) { PutSubprogram(symbol); },
          [&](const GenericDetails &) { PutGeneric(symbol); },
          [&](const UseDetails &) { PutUse(symbol); },
          [&](const UseErrorDetails &) {},
          [&](const auto &) { PutEntity(decls_, symbol); }},
      symbol.details());
}

void ModFileWriter::PutDerivedType(const Symbol &typeSymbol) {
  PutAttrs(decls_ << "type", typeSymbol.attrs(), ","s, ""s);
  PutLower(decls_ << "::", typeSymbol) << '\n';
  PutSymbols(*typeSymbol.scope());
  decls_ << "end type\n";
}

void ModFileWriter::PutSubprogram(const Symbol &symbol) {
  auto attrs{symbol.attrs()};
  Attrs bindAttrs{};
  if (attrs.test(Attr::BIND_C)) {
    // bind(c) is a suffix, not prefix
    bindAttrs.set(Attr::BIND_C, true);
    attrs.set(Attr::BIND_C, false);
  }
  bool isExternal{attrs.test(Attr::EXTERNAL)};
  std::ostream &os{isExternal ? decls_ : contains_};
  if (isExternal) {
    os << "interface\n";
  }
  PutAttrs(os, attrs, ""s, " "s);
  auto &details{symbol.get<SubprogramDetails>()};
  os << (details.isFunction() ? "function " : "subroutine ");
  PutLower(os, symbol) << '(';
  int n = 0;
  for (const auto &dummy : details.dummyArgs()) {
    if (n++ > 0) os << ',';
    PutLower(os, *dummy);
  }
  os << ')';
  PutAttrs(os, bindAttrs, " "s, ""s);
  if (details.isFunction()) {
    const Symbol &result{details.result()};
    if (result.name() != symbol.name()) {
      PutLower(os << " result(", result) << ')';
    }
    os << '\n';
    PutEntity(os, details.result());
  } else {
    os << '\n';
  }
  for (const auto &dummy : details.dummyArgs()) {
    PutEntity(os, *dummy);
  }
  os << "end\n";
  if (isExternal) {
    os << "end interface\n";
  }
}

void ModFileWriter::PutGeneric(const Symbol &symbol) {
  auto &details{symbol.get<GenericDetails>()};
  decls_ << "generic";
  PutAttrs(decls_, symbol.attrs());
  PutLower(decls_ << "::", symbol) << "=>";
  int n = 0;
  for (auto *specific : details.specificProcs()) {
    if (n++ > 0) decls_ << ',';
    PutLower(decls_, *specific);
  }
  decls_ << '\n';
}

void ModFileWriter::PutUse(const Symbol &symbol) {
  auto &details{symbol.get<UseDetails>()};
  auto &use{details.symbol()};
  PutLower(uses_ << "use ", details.module());
  PutLower(uses_ << ",only:", symbol);
  if (use.name() != symbol.name()) {
    PutLower(uses_ << "=>", use);
  }
  uses_ << '\n';
  PutUseExtraAttr(Attr::VOLATILE, symbol, use);
  PutUseExtraAttr(Attr::ASYNCHRONOUS, symbol, use);
}

// We have "USE local => use" in this module. If attr was added locally
// (i.e. on local but not on use), also write it out in the mod file.
void ModFileWriter::PutUseExtraAttr(
    Attr attr, const Symbol &local, const Symbol &use) {
  if (local.attrs().test(attr) && !use.attrs().test(attr)) {
    PutLower(useExtraAttrs_, AttrToString(attr)) << "::";
    PutLower(useExtraAttrs_, local) << '\n';
  }
}

void PutEntity(std::ostream &os, const Symbol &symbol) {
  std::visit(
      common::visitors{
          [&](const EntityDetails &) { PutObjectEntity(os, symbol); },
          [&](const ObjectEntityDetails &) { PutObjectEntity(os, symbol); },
          [&](const ProcEntityDetails &) { PutProcEntity(os, symbol); },
          [&](const auto &) {
            common::die("PutEntity: unexpected details: %s",
                DetailsToString(symbol.details()).c_str());
          },
      },
      symbol.details());
}

void PutObjectEntity(std::ostream &os, const Symbol &symbol) {
  PutEntity(os, symbol, [&]() {
    auto *type{symbol.GetType()};
    CHECK(type);
    PutLower(os, *type);
  });
}

void PutProcEntity(std::ostream &os, const Symbol &symbol) {
  const ProcInterface &interface{symbol.get<ProcEntityDetails>().interface()};
  PutEntity(os, symbol, [&]() {
    os << "procedure(";
    if (interface.symbol()) {
      PutLower(os, *interface.symbol());
    } else if (interface.type()) {
      PutLower(os, *interface.type());
    }
    os << ')';
  });
}

// Write an entity (object or procedure) declaration.
// writeType is called to write out the type.
void PutEntity(
    std::ostream &os, const Symbol &symbol, std::function<void()> writeType) {
  writeType();
  PutAttrs(os, symbol.attrs());
  PutLower(os << "::", symbol) << '\n';
}

// Put out each attribute to os, surrounded by `before` and `after` and
// mapped to lower case.
std::ostream &PutAttrs(
    std::ostream &os, Attrs attrs, std::string before, std::string after) {
  attrs.set(Attr::PUBLIC, false);  // no need to write PUBLIC
  attrs.set(Attr::EXTERNAL, false);  // no need to write EXTERNAL
  for (std::size_t i{0}; i < Attr_enumSize; ++i) {
    Attr attr{static_cast<Attr>(i)};
    if (attrs.test(attr)) {
      PutLower(os << before, AttrToString(attr)) << after;
    }
  }
  return os;
}

std::ostream &PutLower(std::ostream &os, const Symbol &symbol) {
  return PutLower(os, symbol.name().ToString());
}

std::ostream &PutLower(std::ostream &os, const DeclTypeSpec &type) {
  std::stringstream s;
  s << type;
  return PutLower(os, s.str());
}

std::ostream &PutLower(std::ostream &os, const std::string &str) {
  for (char c : str) {
    os << parser::ToLowerCaseLetter(c);
  }
  return os;
}

// Compute a simple hash of the contents of a module file and
// return it as a string of hex digits.
// This uses the Fowler-Noll-Vo hash function.
std::string CheckSum(const std::string &str) {
  std::uint64_t hash{0xcbf29ce484222325ull};
  for (char c : str) {
    hash ^= c & 0xff;
    hash *= 0x100000001b3;
  }
  static const char *digits = "0123456789abcdef";
  std::string result(16, '0');
  for (size_t i{16}; hash != 0; hash >>= 4) {
    result[--i] = digits[hash & 0xf];
  }
  return result;
}

Scope *ModFileReader::Read(const SourceName &name, Scope *ancestor) {
  std::string ancestorName;  // empty for module
  if (ancestor) {
    if (auto *scope{ancestor->FindSubmodule(name)}) {
      return scope;
    }
    ancestorName = ancestor->name().ToString();
  } else {
    auto it{Scope::globalScope.find(name)};
    if (it != Scope::globalScope.end()) {
      return it->second->scope();
    }
  }
  auto path{FindModFile(name, ancestorName)};
  if (!path.has_value()) {
    return nullptr;
  }
  // TODO: Construct parsing with an AllSources reference to share provenance
  parser::Parsing parsing;
  parser::Options options;
  options.isModuleFile = true;
  parsing.Prescan(*path, options);
  parsing.Parse(&std::cout);
  auto &parseTree{parsing.parseTree()};
  if (!parsing.messages().empty() || !parsing.consumedWholeFile() ||
      !parseTree.has_value()) {
    errors_.push_back(
        Error(name, "Module file for '%s' is corrupt: %s"_err_en_US,
            name.ToString(), *path));
    return nullptr;
  }
  Scope *parentScope;  // the scope this module/submodule goes into
  if (!ancestor) {
    parentScope = &Scope::globalScope;
  } else if (auto *parent{GetSubmoduleParent(*parseTree)}) {
    parentScope = Read(*parent, ancestor);
  } else {
    parentScope = ancestor;
  }
  ResolveNames(*parentScope, *parseTree, parsing.cooked(), directories_);
  const auto &it{parentScope->find(name)};
  if (it == parentScope->end()) {
    return nullptr;
  }
  auto &modSymbol{*it->second};
  // TODO: Preserve the CookedSource rather than acquiring its string.
  modSymbol.scope()->set_chars(std::string{parsing.cooked().AcquireData()});
  modSymbol.set(Symbol::Flag::ModFile);
  return modSymbol.scope();
}

std::optional<std::string> ModFileReader::FindModFile(
    const SourceName &name, const std::string &ancestor) {
  std::vector<parser::Message> errors;
  for (auto &dir : directories_) {
    std::string path{ModFilePath(dir, name, ancestor)};
    std::ifstream ifstream{path};
    if (!ifstream.good()) {
      errors.push_back(
          Error(name, "%s: %s"_en_US, path, std::string{std::strerror(errno)}));
    } else {
      std::string line;
      ifstream >> line;
      if (std::equal(line.begin(), line.end(), std::string{magic}.begin())) {
        // TODO: verify rest of header line: version, checksum, etc.
        return path;
      }
      errors.push_back(Error(name, "%s: Not a valid module file"_en_US, path));
    }
  }
  auto error{Error(name,
      ancestor.empty()
          ? "Cannot find module file for '%s'"_err_en_US
          : "Cannot find module file for submodule '%s' of module '%s'"_err_en_US,
      name.ToString(), ancestor)};
  for (auto &e : errors) {
    error.Attach(e);
  }
  errors_.push_back(error);
  return std::nullopt;
}

// program was read from a .mod file for a submodule; return the name of the
// submodule's parent submodule, nullptr if none.
static const SourceName *GetSubmoduleParent(const parser::Program &program) {
  CHECK(program.v.size() == 1);
  auto &unit{program.v.front()};
  auto &submod{std::get<common::Indirection<parser::Submodule>>(unit.u)};
  auto &stmt{std::get<parser::Statement<parser::SubmoduleStmt>>(submod->t)};
  auto &parentId{std::get<parser::ParentIdentifier>(stmt.statement.t)};
  if (auto &parent{std::get<std::optional<parser::Name>>(parentId.t)}) {
    return &parent->source;
  } else {
    return nullptr;
  }
}

// Construct the path to a module file. ancestorName not empty means submodule.
static std::string ModFilePath(const std::string &dir, const SourceName &name,
    const std::string &ancestorName) {
  std::stringstream path;
  if (dir != "."s) {
    path << dir << '/';
  }
  if (!ancestorName.empty()) {
    PutLower(path, ancestorName) << '-';
  }
  PutLower(path, name.ToString()) << extension;
  return path.str();
}

static bool MakeReadonly(const std::string &path) {
  struct stat statbuf;
  if (stat(path.c_str(), &statbuf) != 0) {
    return false;
  }
  auto mode{statbuf.st_mode};
  mode &= S_IRUSR | S_IRGRP | S_IROTH;
  return chmod(path.data(), mode) == 0;
}

static parser::Message Error(const SourceName &location,
    parser::MessageFixedText fixedText, const std::string &arg) {
  return parser::Message{
      location, parser::MessageFormattedText{fixedText, arg.data()}};
}
static parser::Message Error(const SourceName &location,
    parser::MessageFixedText fixedText, const std::string &arg1,
    const std::string &arg2) {
  return parser::Message{location,
      parser::MessageFormattedText{fixedText, arg1.data(), arg2.data()}};
}

}  // namespace Fortran::semantics