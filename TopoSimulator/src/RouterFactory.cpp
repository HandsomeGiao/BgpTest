#include "toposim/RouterFactory.hpp"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace toposim {
namespace {

using RouterRegistry = std::unordered_map<std::string, RouterCreator>;

RouterRegistry &registry() {
  static RouterRegistry classes = {
      {kDefaultRouterClassName,
       [](RouterConfig config) {
         return std::make_shared<BgpRouter>(std::move(config));
       }},
  };
  return classes;
}

std::mutex &registryMutex() {
  static std::mutex mutex;
  return mutex;
}

std::string availableClassesTextLocked() {
  std::vector<std::string> names;
  names.reserve(registry().size());
  for (const auto &[name, _] : registry()) {
    names.push_back(name);
  }
  std::sort(names.begin(), names.end());

  std::string text;
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (i != 0) {
      text += ", ";
    }
    text += names[i];
  }
  return text;
}

} // namespace

void registerRouterClass(std::string class_name, RouterCreator creator) {
  if (class_name.empty()) {
    throw std::invalid_argument("Router class name cannot be empty");
  }
  if (!creator) {
    throw std::invalid_argument("Router class creator cannot be empty");
  }

  std::lock_guard lock(registryMutex());
  auto [_, inserted] = registry().emplace(std::move(class_name),
                                          std::move(creator));
  if (!inserted) {
    throw std::runtime_error("Router class is already registered");
  }
}

bool routerClassExists(const std::string &class_name) {
  std::lock_guard lock(registryMutex());
  return registry().contains(class_name);
}

std::vector<std::string> availableRouterClasses() {
  std::lock_guard lock(registryMutex());
  std::vector<std::string> names;
  names.reserve(registry().size());
  for (const auto &[name, _] : registry()) {
    names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  return names;
}

std::shared_ptr<BgpRouter> createRouterByClass(const std::string &class_name,
                                               RouterConfig config) {
  RouterCreator creator;
  {
    std::lock_guard lock(registryMutex());
    const auto it = registry().find(class_name);
    if (it == registry().end()) {
      throw std::runtime_error("Unknown router class '" + class_name +
                               "'. Available classes: " +
                               availableClassesTextLocked());
    }
    creator = it->second;
  }
  return creator(std::move(config));
}

} // namespace toposim
