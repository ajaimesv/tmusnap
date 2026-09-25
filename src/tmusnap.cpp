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
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <sys/wait.h>

#include "json.hpp"

using json = nlohmann::json;

static bool verbose = false;

// ---------- command execution ----------

static void die(const std::string &msg) {
    std::cerr << "error: " << msg << "\n";
    std::exit(1);
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

// Run a shell command, returning its stdout split into lines.
// If status is given, it receives the process exit status.
static std::vector<std::string> capture_lines(const std::string &cmd,
                                              int *status = nullptr) {
    if (verbose)
        std::cout << "> " << cmd << "\n";

    std::string data;
    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe)
        die("popen failed: " + std::string(std::strerror(errno)));

    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), pipe)) > 0)
        data.append(buf, n);
    const int wait = pclose(pipe);

    if (status) {
        if (WIFEXITED(wait))
            *status = WEXITSTATUS(wait);
        else
            *status = -1;
    }

    if (verbose)
        std::cout << data;

    std::vector<std::string> lines;
    std::istringstream iss(data);
    std::string line;
    while (std::getline(iss, line))
        lines.push_back(line);
    return lines;
}

// Run a shell command, ignoring its output, and return the exit status.
static int run_cmd(const std::string &cmd) {
    int status = -1;
    (void)capture_lines(cmd, &status);
    return status;
}

static bool command_exists(const std::string &cmd) {
    return run_cmd("command -v " + cmd + " >/dev/null 2>&1") == 0;
}

static void need_tmux() {
    if (!command_exists("tmux"))
        die("tmux not found in PATH");
}

static std::string iso_time_now() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S%z", &tm);
    return buf;
}

// ---------- snapshot model ----------

// The in-memory form of a snapshot, shared by save and restore.

struct saved_pane {
    int index = -1;
    std::string cwd;
};

struct saved_window {
    int index = -1;
    std::string name;
    std::string layout;
    int active_pane = -1;
    std::vector<saved_pane> panes; // sorted by index
};

struct saved_session {
    std::string name;
    int active_window = -1;
    std::vector<saved_window> windows; // sorted by index
};

static json window_to_json(const saved_window &w) {
    json jw;
    jw["index"] = w.index;
    jw["name"] = w.name;
    jw["layout"] = w.layout;
    if (w.active_pane >= 0)
        jw["active_pane"] = w.active_pane;
    jw["panes"] = json::array();
    for (const auto &p : w.panes) {
        json jp;
        jp["index"] = p.index;
        jp["cwd"] = p.cwd;
        jw["panes"].push_back(jp);
    }
    return jw;
}

static json session_to_json(const saved_session &s) {
    json js;
    js["name"] = s.name;
    if (s.active_window >= 0)
        js["active_window"] = s.active_window;
    js["windows"] = json::array();
    for (const auto &w : s.windows)
        js["windows"].push_back(window_to_json(w));
    return js;
}

static saved_window window_from_json(const json &j) {
    saved_window w;
    w.index = j.value("index", -1);
    w.name = j.value("name", std::string());
    w.layout = j.value("layout", std::string());
    w.active_pane = j.value("active_pane", -1);
    if (j.contains("panes") && j["panes"].is_array()) {
        for (const auto &jp : j["panes"]) {
            saved_pane p;
            p.index = jp.value("index", -1);
            p.cwd = jp.value("cwd", std::string());
            if (p.index >= 0)
                w.panes.push_back(std::move(p));
        }
    }
    std::sort(w.panes.begin(), w.panes.end(),
              [](const saved_pane &a, const saved_pane &b) {
                  return a.index < b.index;
              });
    return w;
}

static saved_session session_from_json(const json &j) {
    saved_session s;
    s.name = j.value("name", std::string());
    s.active_window = j.value("active_window", -1);
    if (j.contains("windows") && j["windows"].is_array()) {
        for (const auto &jw : j["windows"]) {
            saved_window w = window_from_json(jw);
            if (w.index >= 0)
                s.windows.push_back(std::move(w));
        }
    }
    std::sort(s.windows.begin(), s.windows.end(),
              [](const saved_window &a, const saved_window &b) {
                  return a.index < b.index;
              });
    return s;
}

// ---------- save ----------

// tmux escapes control characters in -F output values (the common ones with
// backslash-C escapes like \t, the rest as \NNN octal); undo it so values
// such as a window name with a tab round-trip unchanged.
static std::string unescape_tmux_value(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            out.push_back(s[i]);
            continue;
        }
        int b;
        switch (s[i + 1]) {
        case 'a':
            b = 0x07;
            i += 1;
            break;
        case 'b':
            b = 0x08;
            i += 1;
            break;
        case 'f':
            b = 0x0C;
            i += 1;
            break;
        case 'n':
            b = 0x0A;
            i += 1;
            break;
        case 'r':
            b = 0x0D;
            i += 1;
            break;
        case 't':
            b = 0x09;
            i += 1;
            break;
        case 'v':
            b = 0x0B;
            i += 1;
            break;
        case '\\':
            b = 0x5C;
            i += 1;
            break;
        default:
            if (s[i + 1] >= '0' && s[i + 1] <= '7') {
                // \NNN octal, up to three digits.
                b = s[i + 1] - '0';
                size_t j = i + 2;
                for (int d = 0; d < 2 && j < s.size() && s[j] >= '0' &&
                               s[j] <= '7';
                     d++) {
                    b = b * 8 + (s[j] - '0');
                    j++;
                }
                i = j - 1;
            } else {
                // Escape we cannot reverse; keep it as-is.
                out.push_back(s[i]);
                continue;
            }
            break;
        }
        out.push_back((char)b);
    }
    return out;
}

// Each field of every window and pane is queried in its own -a call, so a
// line holds exactly one value: no separator or escaping is needed and a
// field value can never corrupt the parsing. (Window variables are queried
// with list-windows because in pane scope some of them, like window_layout,
// expand to shortened values.) All calls see the same server state, so line
// i is the same window/pane in every column.

static std::vector<std::vector<std::string>>
capture_listing(const char *subcommand,
                std::initializer_list<std::string> fields) {
    std::vector<std::vector<std::string>> columns;
    for (const auto &field : fields) {
        const std::string cmd = std::string("tmux ") + subcommand + " -a -F " +
                                shell_quote("#{" + field + "}");
        columns.push_back(capture_lines(cmd));
    }
    for (size_t i = 1; i < columns.size(); i++) {
        if (columns[i].size() != columns[0].size())
            die(std::string("inconsistent ") + subcommand +
                " output (tmux server changed mid-capture)");
    }
    return columns;
}

static std::vector<saved_session> capture_sessions() {
    auto win = capture_listing(
        "list-windows",
        {"session_name", "window_index", "window_active", "window_name",
         "window_layout"});
    if (win[0].empty())
        die("no tmux server running (nothing to save)");
    auto pan = capture_listing(
        "list-panes",
        {"session_name", "window_index", "pane_index", "pane_active",
         "pane_current_path"});

    std::vector<saved_session> sessions;
    std::map<std::string, size_t> session_of;
    // (session name, window index) -> (session position, window position).
    std::map<std::pair<std::string, int>, std::pair<size_t, size_t>>
        window_of;

    for (size_t i = 0; i < win[0].size(); i++) {
        const std::string sname = unescape_tmux_value(win[0][i]);
        int windex;
        try {
            windex = std::stoi(win[1][i]);
        } catch (...) {
            continue;
        }

        size_t si;
        auto sit = session_of.find(sname);
        if (sit == session_of.end()) {
            saved_session s;
            s.name = sname;
            si = sessions.size();
            session_of.emplace(sname, si);
            sessions.push_back(std::move(s));
        } else {
            si = sit->second;
        }

        saved_window w;
        w.index = windex;
        w.name = unescape_tmux_value(win[3][i]);
        w.layout = win[4][i];
        if (win[2][i] == "1" && sessions[si].active_window < 0)
            sessions[si].active_window = windex;

        const size_t wi = sessions[si].windows.size();
        sessions[si].windows.push_back(std::move(w));
        window_of.emplace(std::make_pair(sname, windex),
                          std::make_pair(si, wi));
    }

    for (size_t i = 0; i < pan[0].size(); i++) {
        const std::string sname = unescape_tmux_value(pan[0][i]);
        int windex, pindex;
        try {
            windex = std::stoi(pan[1][i]);
            pindex = std::stoi(pan[2][i]);
        } catch (...) {
            continue;
        }
        auto wit = window_of.find(std::make_pair(sname, windex));
        if (wit == window_of.end())
            continue;
        saved_window &w =
            sessions[wit->second.first].windows[wit->second.second];
        if (pan[3][i] == "1" && w.active_pane < 0)
            w.active_pane = pindex;
        saved_pane p;
        p.index = pindex;
        p.cwd = unescape_tmux_value(pan[4][i]);
        w.panes.push_back(std::move(p));
    }

    return sessions;
}

static void save_snapshot() {
    need_tmux();
    auto sessions = capture_sessions();

    json root;
    root["version"] = 1;
    root["created_at"] = iso_time_now();
    root["sessions"] = json::array();
    for (const auto &s : sessions)
        root["sessions"].push_back(session_to_json(s));

    std::cout << root.dump(2) << "\n";
}

// ---------- restore ----------

static bool has_session(const std::string &session) {
    return run_cmd("tmux has-session -t " + shell_quote(session) +
                   " >/dev/null 2>&1") == 0;
}

// Pane indices of a window, sorted ascending.
static std::vector<int> pane_indices(const std::string &target) {
    const std::string cmd =
        "tmux list-panes -t " + shell_quote(target) + " -F " +
        shell_quote("#{pane_index}");
    std::vector<int> idx;
    for (const auto &line : capture_lines(cmd)) {
        if (line.empty())
            continue;
        try {
            idx.push_back(std::stoi(line));
        } catch (...) {
        }
    }
    std::sort(idx.begin(), idx.end());
    return idx;
}

static std::string first_window_index(const std::string &session) {
    const std::string cmd =
        "tmux list-windows -t " + shell_quote(session) + " -F " +
        shell_quote("#{window_index}");
    for (const auto &line : capture_lines(cmd))
        if (!line.empty())
            return line;
    return "";
}

// Run a setup command, dying with `what` if it fails.
static void run_or_die(const std::string &cmd, const std::string &what) {
    if (run_cmd(cmd) != 0)
        die(what);
}

// Create the panes still missing in a fresh window, restoring their cwds.
// The window's initial pane (created with the first window's command)
// already covers the first saved pane. Each split targets the window's last
// pane (highest index) so that panes keep the saved order in their indices;
// the layout applied afterwards assigns its cells in index order.
static void create_missing_panes(const std::string &target,
                                 const saved_window &w) {
    for (int k = 1; k < (int)w.panes.size(); k++) {
        const auto idx = pane_indices(target);
        if (idx.empty())
            return;
        const std::string last = target + "." + std::to_string(idx.back());
        std::string cmd = "tmux split-window -d -t " + shell_quote(last);
        if (!w.panes[k].cwd.empty())
            cmd += " -c " + shell_quote(w.panes[k].cwd);
        (void)run_cmd(cmd + " >/dev/null 2>&1");
    }
}

// The layout string of one window. (list-windows targets the window's
// session, so pick out the row for the wanted index.)
static std::string current_layout(const std::string &session, int window) {
    const std::string cmd =
        "tmux list-windows -t " + shell_quote(session) + " -F " +
        shell_quote("#{window_index} #{window_layout}");
    const std::string want = std::to_string(window);
    for (const auto &line : capture_lines(cmd)) {
        const size_t sp = line.find(' ');
        if (sp != std::string::npos && line.substr(0, sp) == want)
            return line.substr(sp + 1);
    }
    return "";
}

// Apply the saved layout, falling back to tiled if tmux could not apply
// it. tmux rewrites the pane ids and layout checksum when applying a saved
// string (and on some paths reports an error even though it applied), so
// success is judged by comparing the window layout before and after.
static void apply_saved_layout(const std::string &session,
                               const saved_window &w) {
    if (w.layout.empty())
        return;
    const std::string target = session + ":" + std::to_string(w.index);
    const std::string before = current_layout(session, w.index);
    const int rc = run_cmd("tmux select-layout -t " + shell_quote(target) +
                           " " + shell_quote(w.layout) + " >/dev/null 2>&1");
    if (rc == 0 || current_layout(session, w.index) != before)
        return;
    (void)run_cmd("tmux select-layout -t " + shell_quote(target) +
                  " tiled >/dev/null 2>&1");
}

// Restore the active pane. Saved pane indices may not exist in the rebuilt
// window, so map the position of the saved active pane among the sorted
// saved panes onto the sorted indices of the actual panes.
static void restore_active_pane(const std::string &target,
                                const saved_window &w) {
    if (w.active_pane < 0)
        return;
    int pos = -1;
    for (int i = 0; i < (int)w.panes.size(); i++) {
        if (w.panes[i].index == w.active_pane) {
            pos = i;
            break;
        }
    }
    if (pos < 0)
        return;
    const auto actual = pane_indices(target);
    if (pos >= (int)actual.size())
        return;
    const std::string pane = target + "." + std::to_string(actual[pos]);
    (void)run_cmd("tmux select-pane -t " + shell_quote(pane) +
                  " >/dev/null 2>&1");
}

// Rebuild one window: panes, layout, active pane.
static void apply_window(const std::string &session, const saved_window &w) {
    const std::string target = session + ":" + std::to_string(w.index);
    create_missing_panes(target, w);
    apply_saved_layout(session, w);
    restore_active_pane(target, w);
}

// Create a detached session for `s` whose first window matches `w`.
// The window is moved to its saved index if the server's base-index differs.
static void create_session_with_window(const saved_session &s,
                                       const saved_window &w) {
    std::string cmd = "tmux new-session -d -s " + shell_quote(s.name);
    if (!w.name.empty())
        cmd += " -n " + shell_quote(w.name);
    if (!w.panes.empty() && !w.panes.front().cwd.empty())
        cmd += " -c " + shell_quote(w.panes.front().cwd);
    run_or_die(cmd, "failed creating session " + s.name);

    const std::string created = first_window_index(s.name);
    if (created.empty())
        die("failed to determine initial window index for session " + s.name);

    const std::string desired = std::to_string(w.index);
    if (created != desired) {
        run_or_die("tmux move-window -s " +
                      shell_quote(s.name + ":" + created) + " -t " +
                      shell_quote(s.name + ":" + desired),
                   "failed moving initial window to index " + desired);
    }
}

static void restore_session(const saved_session &s, bool force) {
    if (has_session(s.name)) {
        if (force) {
            (void)run_cmd("tmux kill-session -t " + shell_quote(s.name) +
                          " >/dev/null 2>&1");
        } else {
            die("session already exists: " + s.name +
                " (use --force to replace)");
        }
    }

    if (s.windows.empty()) {
        run_or_die("tmux new-session -d -s " + shell_quote(s.name),
                   "failed creating session " + s.name);
        return;
    }

    const saved_window &first = s.windows.front();
    create_session_with_window(s, first);
    apply_window(s.name, first);

    for (size_t i = 1; i < s.windows.size(); i++) {
        const saved_window &w = s.windows[i];
        const std::string target = s.name + ":" + std::to_string(w.index);
        std::string cmd = "tmux new-window -d -t " + shell_quote(target);
        if (!w.name.empty())
            cmd += " -n " + shell_quote(w.name);
        if (!w.panes.empty() && !w.panes.front().cwd.empty())
            cmd += " -c " + shell_quote(w.panes.front().cwd);
        run_or_die(cmd, "failed creating window " + target);
        apply_window(s.name, w);
    }

    if (s.active_window >= 0) {
        (void)run_cmd("tmux select-window -t " +
                          shell_quote(s.name + ":" +
                                      std::to_string(s.active_window)) +
                          " >/dev/null 2>&1");
    }
}

static std::vector<saved_session> parse_snapshot() {
    json root;
    try {
        std::cin >> root;
    } catch (const std::exception &e) {
        die(std::string("failed to parse json from stdin: ") + e.what());
    }

    const int version = root.value("version", 0);
    if (version != 1)
        die("unsupported snapshot version: " + std::to_string(version));
    if (!root.contains("sessions") || !root["sessions"].is_array())
        die("invalid snapshot: missing sessions[]");

    std::vector<saved_session> sessions;
    for (const auto &jsess : root["sessions"]) {
        saved_session s = session_from_json(jsess);
        if (!s.name.empty())
            sessions.push_back(std::move(s));
    }
    return sessions;
}

static void restore_snapshot(bool force) {
    need_tmux();
    for (const auto &s : parse_snapshot())
        restore_session(s, force);
}

// ---------- command line ----------

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
    std::string mode;
    bool force = false;

    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--save") {
            mode = "save";
        } else if (arg == "--restore") {
            mode = "restore";
        } else if (arg == "--verbose") {
            verbose = true;
        } else if (arg == "--force") {
            force = true;
        } else {
            usage();
            return 1;
        }
    }

    if (mode == "save") {
        save_snapshot();
    } else if (mode == "restore") {
        restore_snapshot(force);
    } else {
        usage();
        return 1;
    }
    return 0;
}
