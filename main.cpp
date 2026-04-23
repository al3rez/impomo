// Pomodoro app with tasks, subtasks, and work-history chart.
// Built with Dear ImGui + GLFW + OpenGL3.
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cmath>
#include <string>
#include <vector>
#include <fstream>
#include <map>
#define GL_SILENCE_DEPRECATION
#include <GLFW/glfw3.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

struct SubTask {
    std::string title;
    int  pomodoros_done = 0;
    bool done = false;
};

struct Task {
    std::string title;
    int  pomodoros_done = 0;
    bool done = false;
    std::vector<SubTask> subs;
};

enum Phase { PHASE_IDLE, PHASE_WORK, PHASE_BREAK };

static const int WORK_SECS  = 25 * 60;
static const int BREAK_SECS = 5 * 60;
static const char* TASKS_PATH   = "pomodoro_tasks.txt";
static const char* HISTORY_PATH = "pomodoro_history.txt";
static const char* STATE_PATH   = "pomodoro_state.txt";

static std::vector<Task> g_tasks;
// active focus: either a task (sub == -1) or a specific subtask
static int   g_active_task  = -1;
static int   g_active_sub   = -1;
static Phase g_phase        = PHASE_IDLE;
static bool  g_running      = false;
static double g_phase_end   = 0.0;   // glfwGetTime() target
static double g_remaining   = WORK_SECS;
static int   g_completed_work_sessions = 0;

// daily history: "YYYY-MM-DD" -> pomodoros completed that day
static std::map<std::string, int> g_history;

// inline "add subtask" editor state
static int  g_sub_edit_for = -1;      // task index currently showing input, -1 = none
static char g_sub_edit_buf[256] = "";

// ─────────────────────────────────────────────────────────────────────────
// Persistence
// ─────────────────────────────────────────────────────────────────────────

static void save_tasks() {
    std::ofstream f(TASKS_PATH);
    if (!f) return;
    f << "v2\n";
    for (const auto& t : g_tasks) {
        f << "task|" << (t.done ? 1 : 0) << '|' << t.pomodoros_done << '|' << t.title << '\n';
        for (const auto& s : t.subs) {
            f << "sub|"  << (s.done ? 1 : 0) << '|' << s.pomodoros_done << '|' << s.title << '\n';
        }
    }
}

static void load_tasks() {
    std::ifstream f(TASKS_PATH);
    if (!f) return;
    std::string first;
    if (!std::getline(f, first)) return;

    auto split3 = [](const std::string& s, std::string& a, int& b, std::string& c) {
        size_t p1 = s.find('|');
        size_t p2 = s.find('|', p1 + 1);
        if (p1 == std::string::npos || p2 == std::string::npos) return false;
        a = s.substr(0, p1);
        b = std::atoi(s.substr(p1 + 1, p2 - p1 - 1).c_str());
        c = s.substr(p2 + 1);
        return true;
    };

    if (first == "v2") {
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            size_t bar = line.find('|');
            if (bar == std::string::npos) continue;
            std::string kind = line.substr(0, bar);
            std::string rest = line.substr(bar + 1);
            std::string doneStr, title;
            int pomos = 0;
            if (!split3(rest, doneStr, pomos, title)) continue;
            bool done = (doneStr == "1");
            if (kind == "task") {
                Task t; t.title = title; t.pomodoros_done = pomos; t.done = done;
                g_tasks.push_back(t);
            } else if (kind == "sub" && !g_tasks.empty()) {
                SubTask s; s.title = title; s.pomodoros_done = pomos; s.done = done;
                g_tasks.back().subs.push_back(s);
            }
        }
    } else {
        // legacy v1: each line is "done|pomos|title"
        auto parse_v1 = [&](const std::string& line) {
            size_t p1 = line.find('|');
            size_t p2 = line.find('|', p1 + 1);
            if (p1 == std::string::npos || p2 == std::string::npos) return;
            Task t;
            t.done = line.substr(0, p1) == "1";
            t.pomodoros_done = std::atoi(line.substr(p1 + 1, p2 - p1 - 1).c_str());
            t.title = line.substr(p2 + 1);
            g_tasks.push_back(t);
        };
        if (!first.empty()) parse_v1(first);
        std::string line;
        while (std::getline(f, line)) if (!line.empty()) parse_v1(line);
    }
}

// Persist/restore live timer state so closing the app doesn't lose progress.
static void save_state() {
    std::ofstream f(STATE_PATH);
    if (!f) return;
    f << "phase "     << (int)g_phase << '\n';
    f << "running "   << (g_running ? 1 : 0) << '\n';
    f << "active_task " << g_active_task << '\n';
    f << "active_sub "  << g_active_sub  << '\n';
    f << "sessions "    << g_completed_work_sessions << '\n';
    // Time info: end_epoch only valid when running; else remaining is the paused value.
    time_t now = time(nullptr);
    if (g_running) {
        long end_epoch = (long)now + (long)g_remaining;
        f << "end_epoch " << end_epoch << '\n';
    } else {
        f << "remaining " << (long)g_remaining << '\n';
    }
}

static void load_state() {
    std::ifstream f(STATE_PATH);
    if (!f) return;
    int phase_i = PHASE_IDLE, running_i = 0;
    long end_epoch = 0, remaining = WORK_SECS;
    bool have_end = false;
    std::string key;
    while (f >> key) {
        if      (key == "phase")       f >> phase_i;
        else if (key == "running")     f >> running_i;
        else if (key == "active_task") f >> g_active_task;
        else if (key == "active_sub")  f >> g_active_sub;
        else if (key == "sessions")    f >> g_completed_work_sessions;
        else if (key == "end_epoch")   { f >> end_epoch; have_end = true; }
        else if (key == "remaining")   f >> remaining;
        else { std::string skip; std::getline(f, skip); }
    }
    g_phase = (Phase)phase_i;
    if (g_phase == PHASE_IDLE) {
        g_running = false;
        g_remaining = WORK_SECS;
        return;
    }
    if (running_i && have_end) {
        long now = (long)time(nullptr);
        long left = end_epoch - now;
        if (left <= 0) {
            // phase expired while app was closed — drop to idle, user will decide.
            g_phase = PHASE_IDLE;
            g_running = false;
            g_remaining = WORK_SECS;
        } else {
            g_remaining = (double)left;
            g_phase_end = glfwGetTime() + g_remaining;
            g_running = true;
        }
    } else {
        // was paused
        g_remaining = (double)remaining;
        g_running = false;
    }
}

static void save_history() {
    std::ofstream f(HISTORY_PATH);
    if (!f) return;
    for (const auto& kv : g_history) {
        f << kv.first << ' ' << kv.second << '\n';
    }
}

static void load_history() {
    std::ifstream f(HISTORY_PATH);
    if (!f) return;
    std::string date;
    int count;
    while (f >> date >> count) g_history[date] = count;
}

// ─────────────────────────────────────────────────────────────────────────
// Date helpers
// ─────────────────────────────────────────────────────────────────────────

static std::string format_date(time_t t) {
    // App is single-threaded, so plain localtime() is fine. Using it avoids
    // the POSIX/Windows split (localtime_r vs localtime_s).
    struct tm* tm = std::localtime(&t);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                  tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    return buf;
}

static std::string today_date() { return format_date(time(nullptr)); }

// ─────────────────────────────────────────────────────────────────────────
// Timer logic
// ─────────────────────────────────────────────────────────────────────────

static void record_pomodoro() {
    g_completed_work_sessions++;
    if (g_active_task >= 0 && g_active_task < (int)g_tasks.size()) {
        Task& t = g_tasks[g_active_task];
        if (g_active_sub >= 0 && g_active_sub < (int)t.subs.size())
            t.subs[g_active_sub].pomodoros_done++;
        else
            t.pomodoros_done++;
        save_tasks();
    }
    g_history[today_date()]++;
    save_history();
}

static void start_phase(Phase p) {
    g_phase = p;
    double dur = (p == PHASE_WORK) ? WORK_SECS : (p == PHASE_BREAK) ? BREAK_SECS : 0;
    g_remaining = dur;
    g_phase_end = glfwGetTime() + dur;
    g_running = (p != PHASE_IDLE);
    save_state();
}

static void tick_timer() {
    if (!g_running) return;
    double now = glfwGetTime();
    g_remaining = g_phase_end - now;
    if (g_remaining <= 0.0) {
        if (g_phase == PHASE_WORK) {
            record_pomodoro();
            start_phase(PHASE_BREAK);
        } else if (g_phase == PHASE_BREAK) {
            g_phase = PHASE_IDLE;
            g_running = false;
            g_remaining = WORK_SECS;
            save_state();
        }
    }
}

static void format_time(double secs, char* buf, size_t n) {
    if (secs < 0) secs = 0;
    int s = (int)(secs + 0.5);
    std::snprintf(buf, n, "%02d:%02d", s / 60, s % 60);
}

static void glfw_error_callback(int error, const char* description) {
    std::fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}


// ─────────────────────────────────────────────────────────────────────────
// UI pieces
// ─────────────────────────────────────────────────────────────────────────

static void draw_history_chart() {
    // Last 14 days ending today.
    const int N = 14;
    float values[N] = {0};
    std::string labels[N];
    time_t now = time(nullptr);
    int total = 0, max_v = 0;
    for (int i = 0; i < N; ++i) {
        time_t d = now - (N - 1 - i) * 86400;
        labels[i] = format_date(d);
        auto it = g_history.find(labels[i]);
        int v = (it != g_history.end()) ? it->second : 0;
        values[i] = (float)v;
        total += v;
        if (v > max_v) max_v = v;
    }

    char overlay[64];
    std::snprintf(overlay, sizeof(overlay), "14d total: %d  |  today: %.0f",
                  total, values[N - 1]);

    ImGui::PlotHistogram("##work_hist", values, N, 0, overlay,
                         0.0f, (float)(max_v > 0 ? max_v : 1),
                         ImVec2(-1, 80));

    // Tiny legend: first and last date.
    ImGui::TextDisabled("%s .. %s", labels[0].c_str(), labels[N - 1].c_str());
}

// Returns true if this row was clicked to become active.
static bool draw_task_row(Task& t, int index, int& delete_idx, bool& sub_delete_req,
                         int& sub_delete_parent, int& sub_delete_index) {
    ImGui::PushID(index);
    bool clicked_active = false;

    // Parent task row ----------------------------------------------------
    bool done = t.done;
    if (ImGui::Checkbox("##done", &done)) {
        t.done = done;
        save_tasks();
    }
    ImGui::SameLine();

    bool is_active = (g_active_task == index && g_active_sub == -1);
    ImVec4 col;
    if (is_active)   col = ImVec4(1.00f, 0.80f, 0.30f, 1.0f);
    else if (t.done) col = ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
    else             col = ImVec4(0.92f, 0.92f, 0.92f, 1.0f);

    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(t.title.c_str());
    if (t.done) { ImGui::SameLine(); ImGui::TextDisabled("(done)"); }
    ImGui::PopStyleColor();

    // right-aligned controls: absolute X so adding "(done)" can't push them off-screen
    char pomo_buf[32];
    std::snprintf(pomo_buf, sizeof(pomo_buf), "%d pomos", t.pomodoros_done);
    float btn_w = 60.0f, small_w = 28.0f, add_w = 56.0f;
    float pomo_w = ImGui::CalcTextSize(pomo_buf).x;
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float total_w = pomo_w + add_w + btn_w + small_w + spacing * 3;
    float target_x = ImGui::GetWindowContentRegionMax().x - total_w;
    ImGui::SameLine();
    if (ImGui::GetCursorPosX() < target_x) ImGui::SetCursorPosX(target_x);

    ImGui::TextUnformatted(pomo_buf);
    ImGui::SameLine();
    if (ImGui::Button("+ sub", ImVec2(add_w, 0))) {
        g_sub_edit_for = index;
        g_sub_edit_buf[0] = '\0';
    }
    ImGui::SameLine();
    if (ImGui::Button(is_active ? "Active" : "Focus", ImVec2(btn_w, 0))) {
        g_active_task = index;
        g_active_sub = -1;
        if (g_phase == PHASE_IDLE) start_phase(PHASE_WORK);
        else save_state();
        clicked_active = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("X", ImVec2(small_w, 0))) delete_idx = index;

    // Subtasks -----------------------------------------------------------
    ImGui::Indent(24.0f);
    for (int j = 0; j < (int)t.subs.size(); ++j) {
        SubTask& s = t.subs[j];
        ImGui::PushID(j + 10000);

        bool sdone = s.done;
        if (ImGui::Checkbox("##sdone", &sdone)) {
            s.done = sdone;
            save_tasks();
        }
        ImGui::SameLine();

        bool sub_active = (g_active_task == index && g_active_sub == j);
        ImVec4 scol;
        if (sub_active)   scol = ImVec4(1.00f, 0.80f, 0.30f, 1.0f);
        else if (s.done)  scol = ImVec4(0.55f, 0.55f, 0.55f, 1.0f);
        else              scol = ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, scol);
        ImGui::Text("- %s", s.title.c_str());
        if (s.done) { ImGui::SameLine(); ImGui::TextDisabled("(done)"); }
        ImGui::PopStyleColor();

        char spomo_buf[32];
        std::snprintf(spomo_buf, sizeof(spomo_buf), "%d pomos", s.pomodoros_done);
        float spomo_w = ImGui::CalcTextSize(spomo_buf).x;
        float stotal = spomo_w + btn_w + small_w + spacing * 2;
        float starget_x = ImGui::GetWindowContentRegionMax().x - stotal;
        ImGui::SameLine();
        if (ImGui::GetCursorPosX() < starget_x) ImGui::SetCursorPosX(starget_x);
        ImGui::TextUnformatted(spomo_buf);
        ImGui::SameLine();
        if (ImGui::Button(sub_active ? "Active" : "Focus", ImVec2(btn_w, 0))) {
            g_active_task = index;
            g_active_sub  = j;
            if (g_phase == PHASE_IDLE) start_phase(PHASE_WORK);
            else save_state();
            clicked_active = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("X", ImVec2(small_w, 0))) {
            sub_delete_req = true;
            sub_delete_parent = index;
            sub_delete_index = j;
        }
        ImGui::PopID();
    }

    // inline add-subtask input (only for the currently-targeted task)
    if (g_sub_edit_for == index) {
        ImGui::SetNextItemWidth(-160);
        bool submit = ImGui::InputText("##sub_input", g_sub_edit_buf, sizeof(g_sub_edit_buf),
                                       ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        bool add = ImGui::Button("Add sub", ImVec2(80, 0));
        ImGui::SameLine();
        bool cancel = ImGui::Button("Cancel", ImVec2(70, 0));
        if ((submit || add) && g_sub_edit_buf[0] != '\0') {
            SubTask s; s.title = g_sub_edit_buf;
            t.subs.push_back(s);
            g_sub_edit_buf[0] = '\0';
            g_sub_edit_for = -1;
            save_tasks();
        } else if (cancel) {
            g_sub_edit_for = -1;
        }
    }

    ImGui::Unindent(24.0f);
    ImGui::Separator();
    ImGui::PopID();
    return clicked_active;
}

// ─────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────

int main(int, char**) {
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) return 1;

    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    // App id / WM class — must match .desktop StartupWMClass so compositors
    // pair the window with the launcher icon.
#ifdef GLFW_WAYLAND_APP_ID
    glfwWindowHintString(GLFW_WAYLAND_APP_ID,     "pomodoro");
#endif
#ifdef GLFW_X11_CLASS_NAME
    glfwWindowHintString(GLFW_X11_CLASS_NAME,     "pomodoro");
    glfwWindowHintString(GLFW_X11_INSTANCE_NAME,  "pomodoro");
#endif

    GLFWwindow* window = glfwCreateWindow(820, 760, "🍅 ImPomo", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    // Window icon. Wayland ignores this (uses .desktop Icon=); X11/Windows/macOS
    // pick it up from glfwSetWindowIcon directly.
    {
        int iw, ih, ich;
        unsigned char* ipx = stbi_load("icon.png", &iw, &ih, &ich, 4);
        if (ipx) {
            GLFWimage icon_img; icon_img.width = iw; icon_img.height = ih; icon_img.pixels = ipx;
            glfwSetWindowIcon(window, 1, &icon_img);
            stbi_image_free(ipx);
        }
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    load_tasks();
    load_history();
    load_state();

    char new_task_buf[256] = "";
    ImVec4 clear_color(0.08f, 0.08f, 0.10f, 1.0f);

    while (!glfwWindowShouldClose(window)) {
        // Idle-friendly: when the timer isn't running, block until the user
        // does something. When running, wake at least twice a second so the
        // countdown display stays fresh. Massively cuts CPU/GPU vs PollEvents.
        if (g_running) glfwWaitEventsTimeout(0.5);
        else           glfwWaitEvents();
        tick_timer();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("Pomodoro", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

        // Timer section -----------------------------------------------
        char tbuf[16];
        double show = g_running ? g_remaining
                    : (g_phase == PHASE_BREAK ? BREAK_SECS : WORK_SECS);
        format_time(show, tbuf, sizeof(tbuf));

        const char* phase_label =
            g_phase == PHASE_WORK  ? "WORK" :
            g_phase == PHASE_BREAK ? "BREAK" : "READY";

        // Current focus label
        std::string focus_label = "no active task";
        if (g_active_task >= 0 && g_active_task < (int)g_tasks.size()) {
            const Task& at = g_tasks[g_active_task];
            if (g_active_sub >= 0 && g_active_sub < (int)at.subs.size())
                focus_label = at.title + "  →  " + at.subs[g_active_sub].title;
            else
                focus_label = at.title;
        }

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12, 8));
        ImGui::Text("Phase: %s   |   Sessions: %d   |   Focus: %s",
                    phase_label, g_completed_work_sessions, focus_label.c_str());

        ImGui::SetWindowFontScale(3.5f);
        ImVec2 tsize = ImGui::CalcTextSize(tbuf);
        float cx = ImGui::GetWindowContentRegionMin().x
                 + (ImGui::GetContentRegionAvail().x - tsize.x) * 0.5f;
        ImGui::SetCursorPosX(cx > 0 ? cx : 0);
        ImGui::TextUnformatted(tbuf);
        ImGui::SetWindowFontScale(1.0f);

        double total = (g_phase == PHASE_BREAK) ? BREAK_SECS : WORK_SECS;
        float frac = 0.f;
        if (g_running && total > 0)
            frac = 1.0f - (float)(g_remaining / total);
        ImGui::ProgressBar(frac, ImVec2(-1, 14), "");

        ImGui::Spacing();
        if (g_running) {
            if (ImGui::Button("Pause", ImVec2(100, 32))) { g_running = false; save_state(); }
        } else {
            const char* lbl = (g_phase == PHASE_IDLE) ? "Start Work" : "Resume";
            if (ImGui::Button(lbl, ImVec2(100, 32))) {
                if (g_phase == PHASE_IDLE) start_phase(PHASE_WORK);
                else { g_phase_end = glfwGetTime() + g_remaining; g_running = true; save_state(); }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset", ImVec2(100, 32))) {
            g_phase = PHASE_IDLE; g_running = false; g_remaining = WORK_SECS;
            save_state();
        }
        ImGui::SameLine();
        if (ImGui::Button("Skip", ImVec2(100, 32))) {
            if (g_phase == PHASE_WORK) { record_pomodoro(); start_phase(PHASE_BREAK); }
            else if (g_phase == PHASE_BREAK) {
                g_phase = PHASE_IDLE; g_running = false; g_remaining = WORK_SECS;
                save_state();
            }
        }

        ImGui::PopStyleVar();
        ImGui::Separator();

        // Chart section -----------------------------------------------
        if (ImGui::CollapsingHeader("Work history (last 14 days)",
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            draw_history_chart();
        }
        ImGui::Separator();

        // Tasks section -----------------------------------------------
        ImGui::Text("Tasks");
        ImGui::SetNextItemWidth(-120);
        bool submit = ImGui::InputText("##newtask", new_task_buf, sizeof(new_task_buf),
                                       ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if ((ImGui::Button("Add", ImVec2(110, 0)) || submit) && new_task_buf[0] != '\0') {
            Task t; t.title = new_task_buf;
            g_tasks.push_back(t);
            new_task_buf[0] = '\0';
            save_tasks();
        }

        ImGui::BeginChild("task_list", ImVec2(0, 0), true);
        int delete_idx = -1;
        bool sub_delete_req = false;
        int sub_delete_parent = -1, sub_delete_index = -1;
        for (int i = 0; i < (int)g_tasks.size(); ++i) {
            draw_task_row(g_tasks[i], i, delete_idx,
                          sub_delete_req, sub_delete_parent, sub_delete_index);
        }
        if (delete_idx >= 0) {
            g_tasks.erase(g_tasks.begin() + delete_idx);
            if (g_active_task == delete_idx) { g_active_task = -1; g_active_sub = -1; }
            else if (g_active_task > delete_idx) g_active_task--;
            if (g_sub_edit_for == delete_idx) g_sub_edit_for = -1;
            save_tasks();
        } else if (sub_delete_req && sub_delete_parent >= 0) {
            Task& t = g_tasks[sub_delete_parent];
            if (sub_delete_index >= 0 && sub_delete_index < (int)t.subs.size()) {
                t.subs.erase(t.subs.begin() + sub_delete_index);
                if (g_active_task == sub_delete_parent) {
                    if (g_active_sub == sub_delete_index) g_active_sub = -1;
                    else if (g_active_sub > sub_delete_index) g_active_sub--;
                }
                save_tasks();
            }
        }
        ImGui::EndChild();

        ImGui::End();

        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    save_tasks();
    save_history();
    save_state();
    // glfw's Wayland backend segfaults inside wl_display_disconnect during
    // glfwTerminate() on this setup. All persistent state is already on disk,
    // so skip full cleanup and let the kernel reclaim resources.
    std::_Exit(0);
}
