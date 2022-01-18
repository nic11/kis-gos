#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

struct Args {
  std::string templates_path;
  std::string full_log_path;
  std::string min_log_path;
  bool decode_mode;
  bool force_ovewrite;
};

void ShowUsageAndExit() {
  std::cerr << R"(Usage:
./logmin --templates-path <path> --full-log-path <path> --min-log-path <path>
         [--decode] [--overwrite|--force|-f]

See readme for details and examples.
)";
  std::exit(1);
}

Args ParseArgs(int argc, char** argv) {
  const auto& shift = [&argc, &argv](size_t by) {
    if (argc < by) {
      throw std::runtime_error(std::to_string(argc) + " args left, shift by " +
                               std::to_string(by) + " was requested");
    }
    const char* cur = argv[0];
    argc -= by;
    argv += by;
    return cur;
  };

  bool has_templates_path = false;
  bool has_full_log_path = false;
  bool has_min_log_path = false;
  Args args{};
  shift(1); // skip argv[0]

  while (argc) {
    std::string arg{argv[0]};
    if (arg == "--templates-path") {
      shift(1);
      std::string val{shift(1)};
      has_templates_path = true;
      args.templates_path = val;
    } else if (arg == "--full-log-path") {
      shift(1);
      std::string val{shift(1)};
      has_full_log_path = true;
      args.full_log_path = val;
    } else if (arg == "--min-log-path") {
      shift(1);
      std::string val{shift(1)};
      has_min_log_path = true;
      args.min_log_path = val;
    } else if (arg == "--decode") {
      shift(1);
      args.decode_mode = true;
    } else if (arg == "--overwrite" || arg == "--force" || arg == "-f") {
      shift(1);
      args.force_ovewrite = true;
    } else {
      ShowUsageAndExit();
    }
  }

  if (!has_templates_path || !has_full_log_path || !has_min_log_path) {
    std::cerr << "Missing mandatory arguments.\n\n";
    ShowUsageAndExit();
  }
  
  return args;
}

std::ofstream OpenOutFile(const std::string& path, const Args& args) {
  if (std::filesystem::exists(path) && !args.force_ovewrite) {
    throw std::runtime_error("Output file already exists. Pass --overwrite to write anyway");
  }
  return std::ofstream(path);
}

struct Tape {
  std::string_view input;
  
  void Shift(size_t by) {
    if (input.size() < by) {
      throw std::runtime_error("Can't shift by " + std::to_string(by) +
                               ", only " + std::to_string(input.size()) +" chars left");
    }
    input = input.substr(by);
  }
  
  std::string_view Get() const {
    return input;
  }
};

struct Template {
  struct PartConst {
    std::string s;
  };
  struct PartInt {};
  struct PartString {};

  using PartAny = std::variant<PartConst, PartInt, PartString>;
  std::vector<PartAny> parts;

  static Template Parse(const std::string& line) {
    Tape tape{line};
    if (tape.Get().empty()) {
      throw std::runtime_error("empty line should not reach here!");
    }
    Template tpl;
    tpl.parts.push_back(PartConst{""});
    while (!tape.Get().empty()) {
      if (tape.Get()[0] == '%') {
        if (tape.Get().size() < 2) {
          throw std::runtime_error("bad template: %% is last symbol!");
        }
        switch (tape.Get()[1]) {
          case '%':
            std::get<PartConst>(tpl.parts.back()).s.push_back('%');
            break;
          case 's':
            tpl.parts.push_back(PartString{});
            tpl.parts.push_back(PartConst{""});
            break;
          case 'd':
            tpl.parts.push_back(PartInt{});
            tpl.parts.push_back(PartConst{""});
            break;
          default:
            throw std::runtime_error(std::string{"bad param spec %%"} + tape.Get()[1]);
        }
        tape.Shift(2);
      } else {
        std::get<PartConst>(tpl.parts.back()).s.push_back(tape.Get()[0]);
        tape.Shift(1);
      }
    }
    return tpl;
  }
};

std::vector<Template> ParseTemplates(std::ifstream& inTpls) {
  std::vector<Template> tpls;
  std::string line;
  while (std::getline(inTpls, line)) {
    if (line.empty()) {
      continue;
    }
    tpls.emplace_back(Template::Parse(line));
  }
  return tpls;
}

struct TemplateMatch {
  struct MatchConst {};
  struct MatchInt {
    int val;
  };
  struct MatchString {
    std::string val;
  };
  
  using MatchAny = std::variant<MatchConst, MatchInt, MatchString>;
  std::vector<MatchAny> parts;
  
  struct PartTryMatcher {
    Tape& tape;
    MatchAny& out;

    bool operator()(const Template::PartConst& in) {
      bool ok = tape.Get().substr(0, in.s.length()) == in.s;
      if (ok) {
        tape.Shift(in.s.length());
      }
      out = MatchConst{};
      return ok;
    }
    
    bool operator()(const Template::PartInt& in) {
      size_t pos;
      MatchInt& match = out.emplace<MatchInt>();
      try {
        match.val = std::stoi(std::string{tape.Get()}, &pos);
        tape.Shift(pos);
      } catch (const std::invalid_argument&) {
        return false;
      }
      return true;
    }
    
    bool operator()(const Template::PartString& in) {
      auto& match = out.emplace<MatchString>();
      while (!tape.Get().empty() && tape.Get()[0] != ' ') {
        match.val.push_back(tape.Get()[0]);
        tape.Shift(1);
      }
      return !match.val.empty();
    }
  };
  
  static std::optional<TemplateMatch> TryMatch(const Template& tpl, Tape& tape) {
    TemplateMatch match;
    for (const auto& part : tpl.parts) {
      MatchAny match_part;
      if (!std::visit(PartTryMatcher{tape, match_part}, part)) {
        return std::nullopt;
      }
      match.parts.emplace_back(std::move(match_part));
    }
    return match;
  }
  
  struct MatchedPartMaterilizer {
    Template::PartAny orig_part;

    std::string operator()(const MatchConst&) const {
      return std::get<Template::PartConst>(orig_part).s;
    }
    
    std::string operator()(const MatchInt& in) const {
      return std::to_string(in.val);
    }
    
    std::string operator()(const MatchString& in) const {
      return in.val;
    }
  };
  
  std::string Materialize(const Template& tpl) const {
    if (parts.size() != tpl.parts.size()) {
      std::runtime_error("match parts size and template parts size is different");
    }
    size_t n = parts.size();
    std::string result;
    for (size_t i = 0; i < n; ++i) {
      result += std::visit(MatchedPartMaterilizer{tpl.parts[i]}, parts[i]);
    }
    return result;
  }
  
  struct MatchedPartWriter {
    std::ostream& out;

    void operator()(const MatchConst&) {
      out << 'C';
    }
    
    void operator()(const MatchInt& m) {
      out << 'I' << m.val << '|';
    }
    
    void operator()(const MatchString& m) {
      out << 'S' << m.val.length() << ':' << m.val;
    }
  };
  
  void Write(std::ofstream& out) const {
    for (const auto& part : parts) {
      std::visit(MatchedPartWriter{out}, part);
    }
    out << '\n';
  }
  
  static TemplateMatch Read(std::istream& in) {
    TemplateMatch match;
    char c;
    while (in.get(c), c != '\n') {
      switch (c) {
        case 'C':
          match.parts.emplace_back(MatchConst{});
          break;
        case 'I': {
          MatchInt m;
          in >> m.val;
          match.parts.emplace_back(m);
          if (in.get(c), c != '|') {
            throw std::runtime_error("bad format");
          }
          break;
        }
        case 'S': {
          size_t len;
          in >> len;
          if (in.get(c), c != ':') {
            throw std::runtime_error("bad format");
          }
          MatchString m{std::string(len, '.')};
          in.read(m.val.data(), len);
          match.parts.emplace_back(m);
          break;
        }
      }
    }
    return match;
  }
};

void MatchAndWriteEncoded(const std::string& line, std::ofstream& out,
                          const std::vector<Template>& tpls) {
  for (size_t i = 0; i < tpls.size(); ++i) {
    Tape tape{line};
    const auto match = TemplateMatch::TryMatch(tpls[i], tape);
    if (match) {
      out << i;
      match->Write(out);
      return;
    }
  }
  std::cerr << "WARNING: could not match a template for the log entry:\n"
               "    " << line << "\n  Dropping it.\n";
}

int Encode(const Args& args) {
  std::ifstream inTpls(args.templates_path);
  std::ifstream inFull(args.full_log_path);
  auto outMin = OpenOutFile(args.min_log_path, args);
  if (!inTpls || !inFull || !outMin) {
    throw std::runtime_error("Failed to open something. Check that all in files exist and we can write to out file");
  }
  const auto& tpls = ParseTemplates(inTpls);
  std::string inLine;
  while (std::getline(inFull, inLine)) {
    MatchAndWriteEncoded(inLine, outMin, tpls);
  }
  return 0;
}

int Decode(const Args& args) {
  std::ifstream inTpls(args.templates_path);
  std::ifstream inMin(args.min_log_path);
  auto outFull = OpenOutFile(args.full_log_path, args);
  if (!inTpls || !inMin || !outFull) {
    throw std::runtime_error("Failed to open something. Check that all in files exist and we can write to out file");
  }
  const auto& tpls = ParseTemplates(inTpls);
  size_t tpl_idx;
  while (inMin >> tpl_idx, inMin) {
    auto match = TemplateMatch::Read(inMin);
    outFull << match.Materialize(tpls[tpl_idx]) << '\n' << std::flush;
  }
  return 0;
}

int main(int argc, char** argv) {
  const auto args = ParseArgs(argc, argv);
  
  if (!args.decode_mode) {
    return Encode(args);
  }
  return Decode(args);
}
