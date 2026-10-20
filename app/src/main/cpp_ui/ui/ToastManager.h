#pragma once

#include <string>

namespace Toast {
void Info(const std::string& message);
void Success(const std::string& message);
void Warning(const std::string& message);
void Error(const std::string& message);
int Count();
void Render();
}
