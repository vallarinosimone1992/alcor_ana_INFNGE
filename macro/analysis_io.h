#ifndef ANALYSIS_IO_H
#define ANALYSIS_IO_H

#include <TFile.h>
#include <TSystem.h>
#include <TString.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace analysis_io {
struct InputSpec {
  std::vector<std::string> files;
  std::string tree_name;
};

inline std::string Trim(const std::string &value)
{
  size_t first = 0;
  while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) {
    ++first;
  }
  size_t last = value.size();
  while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1]))) {
    --last;
  }
  return value.substr(first, last - first);
}

inline bool HasTree(const std::string &path, const char *tree_name)
{
  std::unique_ptr<TFile> file(TFile::Open(path.c_str(), "READ"));
  if (!file || file->IsZombie()) {
    return false;
  }
  return file->Get(tree_name) != nullptr;
}

inline bool EndsWith(const std::string &value, const std::string &suffix)
{
  return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline bool LooksLikeRootFile(const std::string &path)
{
  return EndsWith(path, ".root");
}

inline std::string DetectTreeName(const std::string &path)
{
  if (!LooksLikeRootFile(path)) {
    return "";
  }
  if (HasTree(path, "alcor")) {
    return "alcor";
  }
  return "";
}

inline bool IsDirectory(const std::string &path)
{
  void *dir = gSystem->OpenDirectory(path.c_str());
  if (!dir) {
    return false;
  }
  gSystem->FreeDirectory(dir);
  return true;
}

inline std::vector<std::string> CollectDecodedFiles(const std::string &dir)
{
  // Use the same fifo naming convention as the ALCOR readout/decoder tools.
  std::vector<std::string> names = {
      "alcdaq.fifo_0.root",  "alcdaq.fifo_1.root",  "alcdaq.fifo_2.root",  "alcdaq.fifo_3.root",
      "alcdaq.fifo_4.root",  "alcdaq.fifo_5.root",  "alcdaq.fifo_6.root",  "alcdaq.fifo_7.root",
      "alcdaq.fifo_8.root",  "alcdaq.fifo_9.root",  "alcdaq.fifo_10.root", "alcdaq.fifo_11.root",
      "alcdaq.fifo_12.root", "alcdaq.fifo_13.root", "alcdaq.fifo_14.root", "alcdaq.fifo_15.root",
      "alcdaq.fifo_16.root", "alcdaq.fifo_17.root", "alcdaq.fifo_18.root", "alcdaq.fifo_19.root",
      "alcdaq.fifo_20.root", "alcdaq.fifo_21.root", "alcdaq.fifo_22.root", "alcdaq.fifo_23.root",
      "alcdaq.fifo_24.root",
  };
  std::vector<std::string> files;
  files.reserve(names.size());
  for (const auto &name : names) {
    std::string path = dir + "/" + name;
    if (gSystem->AccessPathName(path.c_str())) {
      continue;
    }
    if (!HasTree(path, "alcor")) {
      std::cout << "Skipping " << path << " (missing 'alcor' TTree)" << std::endl;
      continue;
    }
    files.push_back(path);
  }
  return files;
}

inline std::vector<std::string> FindDecodedDirs(const std::string &input, int maxdepth = 3)
{
  std::vector<std::string> decoded_dirs;
  std::string cmd = "find -L '" + input + "' -maxdepth " + std::to_string(maxdepth) +
                    " -type d -name decoded 2>/dev/null";
  TString output = gSystem->GetFromPipe(cmd.c_str());
  std::istringstream iss(output.Data());
  std::string line;
  while (std::getline(iss, line)) {
    if (line.empty()) {
      continue;
    }
    auto files = CollectDecodedFiles(line);
    if (!files.empty()) {
      decoded_dirs.push_back(line);
    }
  }
  std::sort(decoded_dirs.begin(), decoded_dirs.end());
  decoded_dirs.erase(std::unique(decoded_dirs.begin(), decoded_dirs.end()), decoded_dirs.end());
  return decoded_dirs;
}

inline std::vector<std::string> CollectFilesFromList(const std::string &list_path)
{
  std::vector<std::string> files;
  std::ifstream fin(list_path);
  if (!fin) {
    return files;
  }

  std::string line;
  while (std::getline(fin, line)) {
    auto hash = line.find('#');
    if (hash != std::string::npos) {
      line = line.substr(0, hash);
    }
    line = Trim(line);
    if (line.empty()) {
      continue;
    }
    if (!gSystem->AccessPathName(line.c_str()) && !IsDirectory(line)) {
      if (!LooksLikeRootFile(line)) {
        std::cout << "Skipping " << line << " (not a ROOT file)" << std::endl;
        continue;
      }
      if (HasTree(line, "alcor")) {
        files.push_back(line);
      } else {
        std::cout << "Skipping " << line << " (missing 'alcor' TTree)" << std::endl;
      }
      continue;
    }

    auto decoded_files = CollectDecodedFiles(line);
    if (decoded_files.empty()) {
      decoded_files = CollectDecodedFiles(line + "/decoded");
    }
    files.insert(files.end(), decoded_files.begin(), decoded_files.end());
  }

  std::sort(files.begin(), files.end());
  files.erase(std::unique(files.begin(), files.end()), files.end());
  return files;
}

inline InputSpec ResolveInputSpec(const std::string &input)
{
  InputSpec spec;

  if (!gSystem->AccessPathName(input.c_str()) && !IsDirectory(input)) {
    auto tree = DetectTreeName(input);
    if (!tree.empty()) {
      spec.files.push_back(input);
      spec.tree_name = tree;
      return spec;
    }
    auto list_files = CollectFilesFromList(input);
    if (!list_files.empty()) {
      spec.files = std::move(list_files);
      spec.tree_name = "alcor";
      return spec;
    }
  }

  auto decoded_files = CollectDecodedFiles(input);
  if (decoded_files.empty()) {
    decoded_files = CollectDecodedFiles(input + "/decoded");
  }
  if (decoded_files.empty()) {
    auto decoded_dirs = FindDecodedDirs(input);
    if (decoded_dirs.size() == 1) {
      decoded_files = CollectDecodedFiles(decoded_dirs.front());
    } else if (decoded_dirs.size() > 1) {
      std::cout << "Multiple decoded directories found under " << input << ":" << std::endl;
      for (const auto &cand : decoded_dirs) {
        std::cout << "  " << cand << std::endl;
      }
      return spec;
    }
  }
  if (!decoded_files.empty()) {
    spec.files = std::move(decoded_files);
    spec.tree_name = "alcor";
    return spec;
  }

  return spec;
}
}  // namespace analysis_io

#endif
