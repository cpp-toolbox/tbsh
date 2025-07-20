#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <iostream>
#include <queue>
#include <readline/history.h>
#include <readline/readline.h>
#include <regex>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

class DirectoryHistory {
private:
  std::deque<std::string> history;
  size_t current_index = 0;

public:
  void add(const std::string &path) {
    if (history.empty() || history.back() != path) {
      history.push_back(path);
      current_index = history.size() - 1;
    }
  }

  std::string back() {
    if (current_index > 0) {
      current_index--;
      return history[current_index];
    }
    throw std::runtime_error("No previous directory in history");
  }

  std::string forward() {
    if (current_index < history.size() - 1) {
      current_index++;
      return history[current_index];
    }
    throw std::runtime_error("No next directory in history");
  }

  const std::string &current() const { return history[current_index]; }
};

class Shell {
private:
  std::unordered_map<std::string,
                     std::function<void(std::vector<std::string> &)>>
      custom_commands;

public:
  DirectoryHistory dir_history;
  Shell() {
    // Add the initial directory to history
    dir_history.add(fs::current_path().string());
    initialize_readline();
  }

  void initialize_readline() {
    rl_attempted_completion_function = custom_completion;
    rl_bind_key('\t', rl_complete);
    rl_variable_bind("completion-ignore-case", "on");
  }

  static char **custom_completion(const char *text, int start, int end) {
    return rl_completion_matches(text, rl_filename_completion_function);
  }

  std::string upfind(const std::string &dir_name,
                     fs::path start = fs::current_path()) {
    fs::path current = fs::absolute(start);

    while (true) {
      fs::path candidate = current / dir_name;
      if (fs::exists(candidate) && fs::is_directory(candidate)) {
        return candidate.string();
      }
      if (current == current.root_path()) {
        throw std::runtime_error("Directory '" + dir_name +
                                 "' not found upwards from " + start.string());
      }
      current = current.parent_path();
    }
  }

  std::string downfind(const std::string &target_pattern,
                       fs::path start = fs::current_path(),
                       size_t limit = 1000) {
    size_t search_count = 0;
    fs::path current = fs::absolute(start);
    std::queue<fs::path> directories;
    directories.push(current);

    while (!directories.empty() && search_count < limit) {
      fs::path dir = directories.front();
      directories.pop();

      for (const auto &entry : fs::directory_iterator(dir)) {
        if (entry.is_directory()) {
          directories.push(entry.path());
        } else {
          fs::path rel_path = fs::relative(entry.path(), start);
          std::string rel_path_str = rel_path.string();

          std::cout << rel_path_str << " | " << target_pattern << std::endl;
          if (rel_path_str.size() >= target_pattern.size() &&
              rel_path_str.compare(rel_path_str.size() - target_pattern.size(),
                                   target_pattern.size(),
                                   target_pattern) == 0) {
            return entry.path().string();
          }
        }

        search_count++;
        if (search_count >= limit) {
          throw std::runtime_error("Search limit reached, target not found.");
        }
      }
    }

    throw std::runtime_error("Target '" + target_pattern + "' not found.");
  }

  std::string transform_command(const std::string &command) {
    std::regex pattern(R"((<|>)([a-zA-Z0-9_.\-\/]+))");
    std::string result;
    std::sregex_iterator iter(command.begin(), command.end(), pattern);
    std::sregex_iterator end;

    size_t last_pos = 0;

    while (iter != end) {
      std::smatch match = *iter;
      size_t match_pos = match.position(0);

      // Append text before the match
      result += command.substr(last_pos, match_pos - last_pos);

      char direction = match.str(1)[0];
      std::string path_pattern = match.str(2);

      try {
        std::string found_path;
        if (direction == '<') {
          found_path = upfind(path_pattern);
        } else if (direction == '>') {
          found_path = downfind(path_pattern);
        }
        result += found_path;
      } catch (const std::exception &e) {
        std::cerr << "[find error] " << e.what() << std::endl;
        // If not found, keep original text
        result += match.str(0);
      }

      last_pos = match_pos + match.length(0);
      ++iter;
    }

    // Append remainder of the string
    result += command.substr(last_pos);
    return result;
  }

  bool change_directory(const char *path, bool update_history = true) {
    if (chdir(path) == 0) {
      if (update_history) {
        dir_history.add(fs::current_path().string());
      }
      return true;
    }
    perror("chdir failed");
    return false;
  }

  void
  add_custom_command(const std::string &name,
                     std::function<void(std::vector<std::string> &)> func) {
    custom_commands[name] = func;
  }

  void run() {
    while (true) {
      std::string prompt = "tbsh:" + fs::current_path().string() + "$ ";
      char *input = readline(prompt.c_str());

      if (!input) {
        std::cout << std::endl;
        break;
      }

      std::string input_line(input);
      free(input);

      if (input_line.empty())
        continue;

      add_history(input_line.c_str());

      std::string transformed_line = transform_command(input_line);
      if (transformed_line != input_line) {
        std::cout << "[Transformed] " << input_line << " → " << transformed_line
                  << std::endl;
      }

      std::vector<std::string> args;
      char *cstr = strdup(transformed_line.c_str());
      char *token = strtok(cstr, " ");
      while (token != nullptr) {
        args.emplace_back(token);
        token = strtok(nullptr, " ");
      }

      if (args.empty()) {
        free(cstr);
        continue;
      }

      const std::string &command = args[0];

      if (custom_commands.find(command) != custom_commands.end()) {
        try {
          custom_commands[command](args);
        } catch (const std::exception &e) {
          std::cerr << "Error: " << e.what() << std::endl;
        }
        free(cstr);
        continue;
      }

      if (command == "cd") {
        const char *path = args.size() > 1 ? args[1].c_str() : getenv("HOME");
        if (!path)
          path = "/";
        if (change_directory(path)) {
          std::cout << "Changed directory to: " << dir_history.current()
                    << std::endl;
        }
        free(cstr);
        continue;
      }

      if (command == "exit") {
        free(cstr);
        break;
      }

      std::vector<char *> exec_args;
      for (auto &arg : args)
        exec_args.push_back(&arg[0]);
      exec_args.push_back(nullptr); // Add nullptr termination

      pid_t pid = fork();
      if (pid < 0) {
        perror("fork failed");
      } else if (pid == 0) {
        if (execvp(exec_args[0], exec_args.data()) == -1) {
          perror("execvp failed");
          exit(EXIT_FAILURE);
        }
      } else {
        int status;
        waitpid(pid, &status, 0);
      }

      free(cstr);
    }

    std::cout << "Exiting tbsh." << std::endl;
  }
};

int main() {
  Shell shell;

  shell.add_custom_command("bk", [&](std::vector<std::string> &) {
    try {
      std::string prev_dir = shell.dir_history.back();
      if (shell.change_directory(prev_dir.c_str(), false)) {
        std::cout << "Navigated back to: " << prev_dir << std::endl;
      } else {
        std::cerr << "Failed to navigate back" << std::endl;
      }
    } catch (const std::exception &e) {
      std::cerr << "Cannot go back: " << e.what() << std::endl;
    }
  });

  shell.add_custom_command("fw", [&](std::vector<std::string> &) {
    try {
      std::string next_dir = shell.dir_history.forward();
      if (shell.change_directory(next_dir.c_str(), false)) {
        std::cout << "Navigated forward to: " << next_dir << std::endl;
      } else {
        std::cerr << "Failed to navigate forward" << std::endl;
      }
    } catch (const std::exception &e) {
      std::cerr << "Cannot go forward: " << e.what() << std::endl;
    }
  });

  shell.run();
  return 0;
}
