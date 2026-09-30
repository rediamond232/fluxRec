#include "engine/app_entry.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace {

std::string_view basename_of(std::string_view path) {
    const auto slash = path.find_last_of('/');
    if (slash == std::string_view::npos) return path;
    return path.substr(slash + 1);
}

} // namespace

int main(int argc, char** argv) {
    const std::string_view invoked_as = basename_of(argc > 0 && argv[0] ? argv[0] : "");
    const bool named_recorder = invoked_as == "fluxrec-record";
    const bool engine_flag = argc > 1 && std::string_view(argv[1]) == "--engine";
    if (!named_recorder && !engine_flag) return fluxrec_ui_main(argc, argv);

    if (!engine_flag) return fluxrec_record_main(argc, argv);

    std::vector<char*> record_argv;
    record_argv.reserve(static_cast<std::size_t>(argc));
    record_argv.push_back(argv[0]);
    for (int index = 2; index < argc; ++index) record_argv.push_back(argv[index]);
    record_argv.push_back(nullptr);
    return fluxrec_record_main(static_cast<int>(record_argv.size()) - 1, record_argv.data());
}
