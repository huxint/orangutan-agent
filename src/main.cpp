#include <print>
#include <string_view>

#include <oran/bootstrap/application.hpp>

int main(int argc, char* argv[]) {
  orangutan::bootstrap::ApplicationOptions options;
  for (int i = 1; i < argc; ++i) {
    const std::string_view flag{argv[i]};
    if (flag == "--")
      continue;
    if (flag == "--help" || flag == "-h") {
      std::println(
          "Usage: orangutan --config FILE --prompt TEXT [--workspace DIR] [--state DIR] [--session ID] [--agent NAME]");
      return 0;
    }
    std::string* value = nullptr;
    if (flag == "--config")
      value = &options.config_path;
    else if (flag == "--prompt")
      value = &options.prompt;
    else if (flag == "--workspace")
      value = &options.workspace;
    else if (flag == "--state")
      value = &options.state_directory;
    else if (flag == "--session")
      value = &options.session_id;
    else if (flag == "--agent")
      value = &options.agent_key;
    if (!value || ++i == argc) {
      std::println(stderr, "orangutan: invalid argument {}; see --help", flag);
      return 2;
    }
    *value = argv[i];
  }
  if (options.config_path.empty() || options.prompt.empty()) {
    std::println(stderr, "orangutan: --config and --prompt are required; see --help");
    return 2;
  }
  auto result = orangutan::bootstrap::run_application(std::move(options));
  if (!result) {
    std::println(stderr, "orangutan: {}", result.error().message());
    return 1;
  }
  std::println("{}", result->text);
  return 0;
}
