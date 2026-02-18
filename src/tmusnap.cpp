// tmusnap.cpp (C++17) — save/restore tmux sessions/windows/panes layouts
//
// Build:
//   c++ -std=c++17 -O2 -Wall -Wextra -pedantic tmusnap.cpp -o tmusnap
//
// Usage:
//   ./tmusnap --save
//     -> writes JSON snapshot to stdout
//
//   ./tmusnap --restore [--force]
//     -> reads JSON snapshot from stdin
//

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef __unix__
#include <sys/wait.h>
#endif

#include "json.hpp"

bool verbose = false;

using json = nlohmann::json;

static void die(const std::string &msg) {
    std::cerr << "error: " << msg << "\n";
    std::exit(1);
}

static int run_cmd(const std::string &cmd) {
    if (verbose) std::cout << "> " << cmd << "\n";
    int rc = std::system(cmd.c_str());
#ifdef __unix__
    if (WIFEXITED(rc)) {
        if (verbose) std::cout << rc << "\n";
        return WEXITSTATUS(rc);
    }
#endif
    if (verbose) std::cout << rc << "\n";
    return rc;
}

static std::string run_cmd_capture(const std::string &cmd) {
    if (verbose) std::cout << "> " << cmd << "\n";
    
    std::string data;
    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe)
        die("popen failed: " + std::string(std::strerror(errno)));

    char buf[4096];
    while (true) {
        size_t n = std::fread(buf, 1, sizeof(buf), pipe);
        if (n > 0)
            data.append(buf, buf + n);
        if (n < sizeof(buf)) {
            if (std::feof(pipe))
                break;
            if (std::ferror(pipe))
                break;
        }
    }

    (void)pclose(pipe);
    if (verbose) std::cout << data;
    return data;
}

// Shell single-quote escaping: ' -> '\'' .
static std::string shell_quote(const std::string &s) {
    if (s.empty())
        return "''";
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('\'');
    for (char c : s) {
        if (c == '\'')
            out += "'\\''";
        else
            out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

static bool command_exists(const std::string &cmd) {
    return run_cmd("command -v " + cmd + " >/dev/null 2>&1") == 0;
}

static void need_tmux() {
    if (!command_exists("tmux"))
        die("tmux not found in PATH");
}

static std::string rtrim_newlines(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    return s;
}

static std::vector<std::string> split_lines(const std::string &s) {
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string line;
    while (std::getline(iss, line))
        out.push_back(line);
    return out;
}

// Use an unlikely delimiter
static constexpr char tmux_sep = '\t';

static std::vector<std::string> split_fields_char(const std::string &s,
                                                  char delim) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == delim) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

static std::string tmux_fmt(std::initializer_list<std::string> parts) {
    std::string out;
    bool first = true;
    for (const auto &p : parts) {
        if (!first)
            out.push_back(tmux_sep);
        out += p;
        first = false;
    }
    return out;
}

static std::string iso_time_now() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S%z", &tm);
    return buf;
}

static bool has_session(const std::string &session) {
    return run_cmd("tmux has-session -t " + shell_quote(session) +
                   " >/dev/null 2>&1") == 0;
}

static std::string
only_window_index_in_new_session(const std::string &session) {
    std::string cmd = "tmux list-windows -t " + shell_quote(session) + " -F " +
                      shell_quote("#{window_index}");
    auto lines = split_lines(rtrim_newlines(run_cmd_capture(cmd)));
    for (const auto &ln : lines) {
        if (!ln.empty())
            return ln;
    }
    return "";
}

static int pane_count(const std::string &target_session_window) {
    std::string cmd = "tmux list-panes -t " +
                      shell_quote(target_session_window) + " -F " +
                      shell_quote("#{pane_id}");
    auto lines = split_lines(rtrim_newlines(run_cmd_capture(cmd)));
    int c = 0;
    for (const auto &ln : lines)
        if (!ln.empty())
            c++;
    return c;
}

static std::vector<int>
list_pane_indices_sorted(const std::string &target_session_window) {
    std::string cmd = "tmux list-panes -t " +
                      shell_quote(target_session_window) + " -F " +
                      shell_quote("#{pane_index}");
    auto lines = split_lines(rtrim_newlines(run_cmd_capture(cmd)));
    std::vector<int> idx;
    for (const auto &ln : lines) {
        if (ln.empty())
            continue;
        try {
            idx.push_back(std::stoi(ln));
        } catch (...) {
        }
    }
    std::sort(idx.begin(), idx.end());
    return idx;
}

// ---------- SAVE (stdout) ----------
static void save_snapshot_to_stdout() {
    need_tmux();

    if (run_cmd("tmux list-sessions >/dev/null 2>&1") != 0) {
        die("no tmux server running (nothing to save)");
    }

    json root;
    root["version"] = 1;
    root["created_at"] = iso_time_now();
    root["sessions"] = json::array();

    // sessions
    {
        std::string cmd =
            "tmux list-sessions -F " + shell_quote("#{session_name}");
        auto sessions = split_lines(rtrim_newlines(run_cmd_capture(cmd)));

        for (const auto &session : sessions) {
            if (session.empty())
                continue;

            json jsess;
            jsess["name"] = session;
            jsess["windows"] = json::array();

            // active window
            {
                std::string fmt =
                    tmux_fmt({"#{window_index}", "#{window_active}"});
                std::string cmd_aw = "tmux list-windows -t " +
                                     shell_quote(session) + " -F " +
                                     shell_quote(fmt);
                auto lines =
                    split_lines(rtrim_newlines(run_cmd_capture(cmd_aw)));
                for (const auto &ln : lines) {
                    auto f = split_fields_char(ln, tmux_sep);
                    if (f.size() >= 2 && f[1] == "1") {
                        try {
                            jsess["active_window"] = std::stoi(f[0]);
                        } catch (...) {
                        }
                        break;
                    }
                }
            }

            // windows
            {
                std::string fmt = tmux_fmt(
                    {"#{window_index}", "#{window_name}", "#{window_layout}"});
                std::string cmd_w = "tmux list-windows -t " +
                                    shell_quote(session) + " -F " +
                                    shell_quote(fmt);
                auto wlines =
                    split_lines(rtrim_newlines(run_cmd_capture(cmd_w)));

                for (const auto &wl : wlines) {
                    if (wl.empty())
                        continue;
                    auto wf = split_fields_char(wl, tmux_sep);
                    if (wf.size() < 3)
                        continue;

                    const std::string windex_s = wf[0];
                    const std::string wname = wf[1];
                    const std::string wlayout = wf[2];
                    const std::string target_sw = session + ":" + windex_s;

                    json jwin;
                    try {
                        jwin["index"] = std::stoi(windex_s);
                    } catch (...) {
                        continue;
                    }
                    jwin["name"] = wname;
                    jwin["layout"] = wlayout;
                    jwin["panes"] = json::array();

                    // active pane
                    {
                        std::string fmt_p =
                            tmux_fmt({"#{pane_index}", "#{pane_active}"});
                        std::string cmd_ap = "tmux list-panes -t " +
                                             shell_quote(target_sw) + " -F " +
                                             shell_quote(fmt_p);
                        auto plines = split_lines(
                            rtrim_newlines(run_cmd_capture(cmd_ap)));
                        for (const auto &pl : plines) {
                            auto pf = split_fields_char(pl, tmux_sep);
                            if (pf.size() >= 2 && pf[1] == "1") {
                                try {
                                    jwin["active_pane"] = std::stoi(pf[0]);
                                } catch (...) {
                                }
                                break;
                            }
                        }
                    }

                    // panes (ONLY cwd)
                    {
                        std::string fmt_panes =
                            tmux_fmt({"#{pane_index}", "#{pane_current_path}"});
                        std::string cmd_panes = "tmux list-panes -t " +
                                                shell_quote(target_sw) +
                                                " -F " + shell_quote(fmt_panes);
                        auto plines = split_lines(
                            rtrim_newlines(run_cmd_capture(cmd_panes)));

                        for (const auto &pl : plines) {
                            if (pl.empty())
                                continue;
                            auto pf = split_fields_char(pl, tmux_sep);
                            if (pf.size() < 2)
                                continue;

                            json jp;
                            try {
                                jp["index"] = std::stoi(pf[0]);
                            } catch (...) {
                                continue;
                            }
                            jp["cwd"] = pf[1]; // current directory for each
                                               // pane is stored here
                            jwin["panes"].push_back(jp);
                        }
                    }

                    jsess["windows"].push_back(jwin);
                }
            }

            root["sessions"].push_back(jsess);
        }
    }

    std::cout << root.dump(2) << "\n";
}

// ---------- RESTORE (stdin) ----------
struct saved_pane {
    int index = -1;
    std::string cwd;
};

struct saved_window {
    int index = -1;
    std::string name;
    std::string layout;
    int active_pane = -1;
    std::vector<saved_pane> panes; // sorted by pane.index
};

struct saved_session {
    std::string name;
    int active_window = -1;
    std::vector<saved_window> windows; // sorted by window.index
};

static std::vector<saved_session> parse_snapshot_from_stdin() {
    json root;
    try {
        std::cin >> root;
    } catch (const std::exception &e) {
        die(std::string("failed to parse json from stdin: ") + e.what());
    }

    int version = root.value("version", 0);
    if (version != 1)
        die("unsupported snapshot version: " + std::to_string(version));

    if (!root.contains("sessions") || !root["sessions"].is_array()) {
        die("invalid snapshot: missing sessions[]");
    }

    std::vector<saved_session> sessions;

    for (const auto &jsess : root["sessions"]) {
        saved_session ss;
        ss.name = jsess.value("name", "");
        ss.active_window = jsess.value("active_window", -1);
        if (ss.name.empty())
            continue;

        if (jsess.contains("windows") && jsess["windows"].is_array()) {
            for (const auto &jwin : jsess["windows"]) {
                saved_window sw;
                sw.index = jwin.value("index", -1);
                sw.name = jwin.value("name", "");
                sw.layout = jwin.value("layout", "");
                sw.active_pane = jwin.value("active_pane", -1);
                if (sw.index < 0)
                    continue;

                if (jwin.contains("panes") && jwin["panes"].is_array()) {
                    for (const auto &jp : jwin["panes"]) {
                        saved_pane sp;
                        sp.index = jp.value("index", -1);
                        sp.cwd = jp.value("cwd", "");
                        if (sp.index >= 0)
                            sw.panes.push_back(std::move(sp));
                    }
                }

                std::sort(sw.panes.begin(), sw.panes.end(),
                          [](const saved_pane &a, const saved_pane &b) {
                              return a.index < b.index;
                          });

                ss.windows.push_back(std::move(sw));
            }
        }

        std::sort(ss.windows.begin(), ss.windows.end(),
                  [](const saved_window &a, const saved_window &b) {
                      return a.index < b.index;
                  });

        sessions.push_back(std::move(ss));
    }

    return sessions;
}

static void ensure_panes_with_dirs(const std::string &target_sw,
                                   const saved_window &sw) {
    const int desired = (int)sw.panes.size();
    if (desired <= 0)
        return;

    int cur = pane_count(target_sw);

    for (int i = cur; i < desired; i++) {
        std::string cwd = sw.panes[i].cwd;
        std::string cmd = "tmux split-window -d -t " + shell_quote(target_sw);
        if (!cwd.empty())
            cmd += " -c " + shell_quote(cwd);
        cmd += " >/dev/null 2>&1";
        (void)run_cmd(cmd);
    }

    // Apply saved layout (fallback to tiled).
    if (!sw.layout.empty()) {
        std::string cmd = "tmux select-layout -t " + shell_quote(target_sw) +
                          " " + shell_quote(sw.layout) + " >/dev/null 2>&1";
        if (run_cmd(cmd) != 0) {
            (void)run_cmd("tmux select-layout -t " + shell_quote(target_sw) +
                          " tiled >/dev/null 2>&1");
        }
    }

    // Restore active pane by position mapping between sorted saved panes and
    // actual pane indices.
    if (sw.active_pane >= 0) {
        int pos = -1;
        for (int i = 0; i < (int)sw.panes.size(); i++) {
            if (sw.panes[i].index == sw.active_pane) {
                pos = i;
                break;
            }
        }
        if (pos >= 0) {
            auto actual = list_pane_indices_sorted(target_sw);
            if (pos < (int)actual.size()) {
                std::string target =
                    target_sw + "." + std::to_string(actual[pos]);
                (void)run_cmd("tmux select-pane -t " + shell_quote(target) +
                              " >/dev/null 2>&1");
            }
        }
    }
}

static void restore_snapshot_from_stdin(bool force) {
    need_tmux();
    auto sessions = parse_snapshot_from_stdin();

    for (const auto &ss : sessions) {
        if (has_session(ss.name)) {
            if (force) {
                (void)run_cmd("tmux kill-session -t " + shell_quote(ss.name) +
                              " >/dev/null 2>&1");
            } else {
                die("session already exists: " + ss.name +
                    " (use --force to replace)");
            }
        }

        if (ss.windows.empty()) {
            // Nothing to restore; still create an empty session.
            std::string cmd = "tmux new-session -d -s " + shell_quote(ss.name) +
                              " >/dev/null 2>&1";
            if (run_cmd(cmd) != 0)
                die("failed creating session " + ss.name);
            continue;
        }

        // Create the session using the first (lowest-index) window.
        const saved_window &first_w = ss.windows.front();
        std::string first_cwd =
            (!first_w.panes.empty()) ? first_w.panes.front().cwd : "";

        {
            std::string cmd = "tmux new-session -d -s " + shell_quote(ss.name) +
                              " -n " + shell_quote(first_w.name);
            if (!first_cwd.empty())
                cmd += " -c " + shell_quote(first_cwd);
            cmd += " >/dev/null 2>&1";
            if (run_cmd(cmd) != 0)
                die("failed creating session " + ss.name);
        }

        // Move the initially-created window to match the saved window index if
        // needed.
        {
            std::string created_idx = only_window_index_in_new_session(ss.name);
            if (created_idx.empty())
                die("failed to determine initial window index for session " +
                    ss.name);

            const std::string desired_idx = std::to_string(first_w.index);
            if (created_idx != desired_idx) {
                std::string cmd = "tmux move-window -s " +
                                  shell_quote(ss.name + ":" + created_idx) +
                                  " -t " +
                                  shell_quote(ss.name + ":" + desired_idx) +
                                  " >/dev/null 2>&1";
                if (run_cmd(cmd) != 0)
                    die("failed moving initial window to index " + desired_idx);
            }
        }

        // Ensure panes + layout + active pane for the first window.
        {
            const std::string target_sw =
                ss.name + ":" + std::to_string(first_w.index);
            ensure_panes_with_dirs(target_sw, first_w);
        }

        // Create remaining windows.
        for (size_t wi = 1; wi < ss.windows.size(); wi++) {
            const saved_window &sw = ss.windows[wi];
            const std::string widx = std::to_string(sw.index);
            const std::string target_sw = ss.name + ":" + widx;

            std::string cwd0 = (!sw.panes.empty()) ? sw.panes.front().cwd : "";

            {
                std::string cmd = "tmux new-window -d -t " +
                                  shell_quote(target_sw) + " -n " +
                                  shell_quote(sw.name);
                if (!cwd0.empty())
                    cmd += " -c " + shell_quote(cwd0);
                cmd += " >/dev/null 2>&1";
                if (run_cmd(cmd) != 0)
                    die("failed creating window " + target_sw);
            }

            ensure_panes_with_dirs(target_sw, sw);
        }

        // Restore active window.
        if (ss.active_window >= 0) {
            std::string target =
                ss.name + ":" + std::to_string(ss.active_window);
            (void)run_cmd("tmux select-window -t " + shell_quote(target) +
                          " >/dev/null 2>&1");
        }
    }
}

static void usage() {
    std::cerr <<
        R"(Usage:
  tmusnap --save [--verbose]
    Write snapshot JSON to stdout.

  tmusnap --restore [--force] [--verbose]
    Read snapshot JSON from stdin and restore it.
    --force: overwrite any existing sessions with the same names. Be careful.

Examples:
  tmusnap --save > tmux_snapshot.json
  tmusnap --restore < tmux_snapshot.json
)";
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 1;
    }

    bool save = false;
    bool restore = false;
    bool force = false;
    
    for (int i = 1; i < argc; i++) {
        std::string cmd = argv[i];

        if (cmd == "--save") {
            save = true;
            restore = false;
        }
        else if (cmd == "--restore") {
            restore = true;
            save = false;
        }
        else if (cmd == "--verbose") verbose = true;
        else if (cmd == "--force") force = true;
        else {
            usage();
            return 1;
        }
    }

    if (save) {
        save_snapshot_to_stdout();
        return 0;
    } else if (restore) {
        restore_snapshot_from_stdin(force);
        return 0;
    }

    usage();
    return 1;
}
