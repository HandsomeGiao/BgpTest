#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "toposim/BgpRouter.hpp"

namespace toposim {

using RouterCreator = std::function<std::shared_ptr<BgpRouter>(RouterConfig)>;

inline constexpr const char *kDefaultRouterClassName = "BgpRouter";

void registerRouterClass(std::string class_name, RouterCreator creator);
[[nodiscard]] bool routerClassExists(const std::string &class_name);
[[nodiscard]] std::vector<std::string> availableRouterClasses();
[[nodiscard]] std::shared_ptr<BgpRouter>
createRouterByClass(const std::string &class_name, RouterConfig config);

} // namespace toposim
