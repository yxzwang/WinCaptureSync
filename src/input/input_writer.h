#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "input/input_types.h"

namespace wcs::input {

class InputWriter {
public:
    bool Open(const std::filesystem::path& path);
    void Write(const InputEvent& event);
    void WriteBatch(const std::vector<InputEvent>& events);
    void Flush();
    void Close();

private:
    std::string Serialize(const InputEvent& event) const;
    std::ofstream file_;
};

}  // namespace wcs::input
