// Pocket TTS Desktop checkpoint B. Optional raylib frontend on Linux.
// Original Kyutai/Python TTS inference is delegated to a local pipe worker.
// MIT license, see repository LICENSE.
#include "raylib.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
namespace {
constexpr int WINDOW_WIDTH = 820;
constexpr int WINDOW_HEIGHT = 680;
const Color BACKGROUND = {246, 247, 250, 255};
const Color INK = {33, 41, 56, 255};
const Color SUBTLE = {89, 100, 116, 255};
const Color ACCENT = {40, 92, 213, 255};
const Color FRAME = {208, 216, 228, 255};
const Color SURFACE = {255, 255, 255, 255};

void drop_last_char(std::string& value) {
    if (value.empty()) return;
    size_t index = value.size() - 1;
    while (index > 0 && (static_cast<unsigned char>(value[index]) & 0xc0) == 0x80)
        --index;
    value.erase(index);
}

void append_codepoint(std::string& value, int codepoint, size_t max_bytes) {
    if (codepoint < 32 || codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff))
        return;
    char bytes[4];
    int n = 0;
    if (codepoint < 0x80) bytes[n++] = static_cast<char>(codepoint);
    else if (codepoint < 0x800) {
        bytes[n++] = static_cast<char>(0xc0 | (codepoint >> 6));
        bytes[n++] = static_cast<char>(0x80 | (codepoint & 63));
    } else if (codepoint < 0x10000) {
        bytes[n++] = static_cast<char>(0xe0 | (codepoint >> 12));
        bytes[n++] = static_cast<char>(0x80 | ((codepoint >> 6) & 63));
        bytes[n++] = static_cast<char>(0x80 | (codepoint & 63));
    } else {
        bytes[n++] = static_cast<char>(0xf0 | (codepoint >> 18));
        bytes[n++] = static_cast<char>(0x80 | ((codepoint >> 12) & 63));
        bytes[n++] = static_cast<char>(0x80 | ((codepoint >> 6) & 63));
        bytes[n++] = static_cast<char>(0x80 | (codepoint & 63));
    }
    if (value.size() + static_cast<size_t>(n) <= max_bytes) value.append(bytes, n);
}

void edit_text(std::string& value, bool multiline, size_t limit) {
    const bool control = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    if (control && IsKeyPressed(KEY_A)) value.clear(); // Simple editor: clear all.
    if (control && IsKeyPressed(KEY_V)) {
        if (const char* clipboard = GetClipboardText()) {
            for (const char* p = clipboard; *p && value.size() < limit; ++p)
                if (multiline || (*p != '\r' && *p != '\n')) value.push_back(*p);
        }
    }
    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE))
        drop_last_char(value);
    if (multiline && !control && IsKeyPressed(KEY_ENTER) && value.size() < limit)
        value.push_back('\n');
    int ch;
    while ((ch = GetCharPressed()) != 0) {
        if (!control) append_codepoint(value, ch, limit);
    }
}

std::string short_path(const std::string& str, int max_width, int size) {
    if (MeasureText(str.c_str(), size) <= max_width) return str;
    size_t offset = 0;
    while (offset < str.size() &&
           MeasureText(("..." + str.substr(offset)).c_str(), size) > max_width)
        ++offset;
    return "..." + str.substr(offset);
}

void draw_multiline(const std::string& str, Rectangle rect, int font_size) {
    const int line_height = font_size + 7;
    const int max_lines = std::max(1, static_cast<int>(rect.height) / line_height);
    std::vector<std::string> lines(1);
    for (size_t i = 0; i < str.size(); ++i) {
        if (str[i] == '\n') {
            lines.emplace_back();
            continue;
        }
        const unsigned char ch = static_cast<unsigned char>(str[i]);
        size_t n = (ch & 0xe0) == 0xc0 ? 2 : (ch & 0xf0) == 0xe0 ? 3 : (ch & 0xf8) == 0xf0 ? 4 : 1;
        n = std::min(n, str.size() - i);
        const auto cp = str.substr(i, n);
        if (!lines.back().empty() &&
            MeasureText((lines.back() + cp).c_str(), font_size) > rect.width)
            lines.emplace_back();
        lines.back() += cp;
        i += n - 1;
    }
    const int start = std::max(0, static_cast<int>(lines.size()) - max_lines);
    BeginScissorMode(static_cast<int>(rect.x), static_cast<int>(rect.y),
                     static_cast<int>(rect.width), static_cast<int>(rect.height));
    for (int i = start; i < static_cast<int>(lines.size()); ++i)
        DrawText(lines[i].c_str(), static_cast<int>(rect.x),
                 static_cast<int>(rect.y) + (i - start)*line_height, font_size, INK);
    EndScissorMode();
}

bool button(Rectangle r, const char* label, bool enabled, bool prominent = false) {
    const bool hovered = enabled && CheckCollisionPointRec(GetMousePosition(), r);
    const Color fill = !enabled ? FRAME :
        prominent ? (hovered ? Color{23, 70, 177, 255} : ACCENT) :
        (hovered ? Color{232, 237, 249, 255} : SURFACE);
    DrawRectangleRounded(r, 0.14f, 8, fill);
    if (!prominent) DrawRectangleRoundedLinesEx(r, 0.14f, 8, 1.0f, FRAME);
    const int font = 17;
    DrawText(label, static_cast<int>(r.x + (r.width - MeasureText(label, font))/2),
             static_cast<int>(r.y + (r.height - font)/2),
             font, prominent ? SURFACE : INK);
    return hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

// Zenity is optional; constant commands with no interpolated user input.
std::string dialog(bool save) {
    const char* command = save ?
        "zenity --file-selection --save --confirm-overwrite --title='Save speech WAV' --filename='pocket-tts-output.wav' 2>/dev/null" :
        "zenity --file-selection --title='Choose local reference' --file-filter='Audio | *.wav *.mp3 *.flac *.ogg *.safetensors' 2>/dev/null";
    FILE* pipe = popen(command, "r");
    if (!pipe) return {};
    std::string result;
    char buffer[2048];
    while (fgets(buffer, sizeof buffer, pipe)) result += buffer;
    if (pclose(pipe) != 0) return {};
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    return result;
}

bool write_fully(FILE* pipe, const std::string& str) {
    return fwrite(str.data(), 1, str.size(), pipe) == str.size();
}

class PythonWorker {
    std::atomic<pid_t> child_{-1};
    FILE* send_ = nullptr;
    FILE* receive_ = nullptr;

    static bool read_exact(FILE* stream, std::string& destination, size_t length) {
        destination.resize(length);
        return length == 0 ||
            fread(destination.data(), 1, length, stream) == length;
    }

public:
    bool start(const std::string& python, std::string& error) {
        if (child_ > 0) return send_ && receive_;
        int to_child[2] = {-1, -1}, from_child[2] = {-1, -1};
        if (pipe(to_child) || pipe(from_child)) {
            error = "Could not open local engine pipes.";
            if (to_child[0] >= 0) { close(to_child[0]); close(to_child[1]); }
            if (from_child[0] >= 0) { close(from_child[0]); close(from_child[1]); }
            return false;
        }
        pid_t pid = fork();
        if (pid < 0) {
            error = "Could not start Python process.";
            for (int fd : {to_child[0], to_child[1], from_child[0], from_child[1]}) close(fd);
            return false;
        }
        if (pid == 0) {
            dup2(to_child[0], STDIN_FILENO);
            dup2(from_child[1], STDOUT_FILENO);
            for (int fd : {to_child[0], to_child[1], from_child[0], from_child[1]})
                close(fd);
            // No shell and no network listener. The Python worker explicitly
            // enables Hugging Face offline mode before model imports.
            execlp(python.c_str(), python.c_str(), "-u", "-m",
                   "pocket_tts.desktop.worker", static_cast<char*>(nullptr));
            _exit(127);
        }
        child_ = pid;
        close(to_child[0]);
        close(from_child[1]);
        send_ = fdopen(to_child[1], "wb");
        receive_ = fdopen(from_child[0], "rb");
        if (!send_ || !receive_) {
            error = "Could not attach local engine streams.";
            if (!send_) close(to_child[1]);
            if (!receive_) close(from_child[0]);
            stop();
            return false;
        }
        setvbuf(send_, nullptr, _IONBF, 0);
        char ready[16] = {};
        if (!fgets(ready, sizeof ready, receive_) || std::strcmp(ready, "READY\n") != 0) {
            error = "Python engine did not start. Check terminal for missing dependencies.";
            return false;
        }
        return true;
    }

    bool generate(const fs::path& voice, const std::string& text,
                  const fs::path& out, std::string& error) {
        const std::string v=voice.string(), o=out.string();
        if (v.size() > 4096 || text.size() > 12000 || o.size() > 4096) {
            error = "Reference, text or output path is too long.";
            return false;
        }
        const std::string head = "GEN " + std::to_string(v.size()) + " " +
            std::to_string(text.size()) + " " + std::to_string(o.size()) + "\n";
        if (!write_fully(send_, head) || !write_fully(send_, v) ||
            !write_fully(send_, text) || !write_fully(send_, o) || fflush(send_) != 0) {
            error = "Could not send request to the local Python engine.";
            stop();
            return false;
        }
        char reply[128] = {};
        if (!fgets(reply, sizeof reply, receive_)) {
            error = "Python worker exited unexpectedly; check terminal.";
            stop();
            return false;
        }
        if (std::strcmp(reply, "OK\n") == 0) return true;
        size_t length = 0;
        char trailing = 0;
        if (std::sscanf(reply, "ERR %zu%c", &length, &trailing) >= 1 && length <= 8192) {
            if (!read_exact(receive_, error, length)) error = "Incomplete engine error response.";
            return false;
        }
        error = "Unexpected response from Python engine.";
        return false;
    }

    pid_t pid() const { return child_.load(); }

    void stop() {
        const pid_t pid = child_.exchange(-1);
        if (pid <= 0) return;
        kill(pid, SIGTERM);
        if (send_) { fclose(send_); send_ = nullptr; }
        if (receive_) { fclose(receive_); receive_ = nullptr; }
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    }

    ~PythonWorker() { stop(); }
};

struct EngineJob {
    PythonWorker worker;
    std::thread thread;
    std::mutex guard;
    std::string error;
    std::atomic<bool> busy{false};
    std::atomic<bool> done{false};
};

std::string python_executable(const fs::path& root) {
    if (const char* p = getenv("POCKET_TTS_PYTHON"); p && *p) return p;
    if (const char* env = getenv("VIRTUAL_ENV"); env && *env) {
        const fs::path active = fs::path(env) / "bin/python";
        if (fs::is_regular_file(active)) return active.string();
    }
    const fs::path local = root / ".venv/bin/python";
    if (fs::exists(local)) return local.string();
    return "python3";
}

bool valid_reference(const fs::path& path) {
    std::string ext=path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".wav" && ext != ".mp3" && ext != ".flac" &&
        ext != ".ogg" && ext != ".safetensors") return false;
    std::error_code ec;
    return fs::is_regular_file(path, ec) && fs::file_size(path, ec) > 0 && !ec;
}
} // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    signal(SIGPIPE, SIG_IGN);
    const fs::path root = fs::absolute(fs::current_path());
    const std::string python = python_executable(root);
    const fs::path wav = root / "desktop-output" / "latest.wav";
    std::error_code ec;
    fs::create_directories(wav.parent_path(), ec);
    if (ec) { std::fprintf(stderr, "Unable to create local output folder: %s\n", ec.message().c_str()); return 1; }

    InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "Pocket TTS Desktop - Local");
    SetWindowMinSize(750, 650);
    InitAudioDevice();
    SetTargetFPS(60);

    EngineJob job;
    std::string text = "Hello. This is Pocket TTS running locally.";
    std::string voice;
    std::string status = "Select or drag in a local reference audio file.";
    bool edit_main = false, edit_voice = false, loaded = false;
    Sound sound = {};
    bool was_playing = false;

    while (!WindowShouldClose()) {
        const int width = GetScreenWidth(), height = GetScreenHeight();
        const float x = 35, w = width - 70.0f;
        const Rectangle text_box = {x, 102, w, 176};
        const Rectangle voice_box = {x, 334, w - 163, 46};
        const Rectangle generate_box = {x, 413, w, 49};
        const Rectangle play_box = {x, 531, 145, 44};
        const Rectangle save_box = {x+161, 531, 174, 44};

        if (IsFileDropped()) {
            FilePathList dropped = LoadDroppedFiles();
            if (dropped.count > 0) {
                voice = dropped.paths[0];
                status = "Reference selected.";
            }
            UnloadDroppedFiles(dropped);
        }
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            const Vector2 point=GetMousePosition();
            edit_main = CheckCollisionPointRec(point, text_box);
            edit_voice = CheckCollisionPointRec(point, voice_box);
        }
        if (!job.busy) {
            if (edit_main) edit_text(text, true, 8000);
            else if (edit_voice) edit_text(voice, false, 4096);
        }

        if (job.done.exchange(false)) {
            if (job.thread.joinable()) job.thread.join();
            std::string error;
            { std::lock_guard<std::mutex> lock(job.guard); error=job.error; }
            if (!error.empty()) status = error;
            else {
                if (loaded) { StopSound(sound); UnloadSound(sound); loaded = false; }
                sound = LoadSound(wav.string().c_str());
                loaded = IsSoundValid(sound);
                status = loaded ? "Generated speech is ready. Press Play or Save." :
                    "WAV created, but raylib could not read it.";
            }
        }

        was_playing = loaded && IsSoundPlaying(sound);
        const bool can_generate = !job.busy && !voice.empty() && !text.empty();
        BeginDrawing();
        ClearBackground(BACKGROUND);
        DrawText("Pocket TTS", int(x), 26, 29, INK);
        DrawText("Local desktop  -  original Python synthesis", int(x), 65, 16, SUBTLE);
        DrawText("Text to speak", int(x), 84, 16, INK);
        DrawRectangleRounded(text_box, 0.05f, 8, SURFACE);
        DrawRectangleRoundedLinesEx(text_box, 0.05f, 8, 1.2f, edit_main ? ACCENT : FRAME);
        draw_multiline(text, {x + 12, 112, w-24, 157}, 18);

        DrawText("Reference audio  (WAV / MP3 / FLAC / OGG / safetensors)", int(x), 309, 15, INK);
        DrawRectangleRounded(voice_box, 0.12f, 8, SURFACE);
        DrawRectangleRoundedLinesEx(voice_box, 0.12f, 8, 1.2f, edit_voice ? ACCENT : FRAME);
        DrawText(voice.empty() ? "Drag file or paste its path..." :
                 short_path(voice, static_cast<int>(voice_box.width)-22, 16).c_str(),
                 int(x+12), 348, 16, voice.empty() ? SUBTLE : INK);
        if (button({x + w-147, 334, 147, 46}, "Browse...", !job.busy)) {
            std::string picked = dialog(false);
            if (!picked.empty()) { voice=picked; status="Reference selected."; }
            else status="Picker cancelled or zenity missing. Drag or paste a path.";
        }
        if (button(generate_box, job.busy ? "Generating audio..." : "Generate Audio", can_generate, true)) {
            if (!valid_reference(voice)) {
                status = "Reference must be an existing local WAV/MP3/FLAC/OGG/safetensors file.";
            } else {
                if (loaded) { StopSound(sound); UnloadSound(sound); loaded = false; }
                status = "Generating using original Pocket TTS...";
                std::string input=text;
                fs::path reference=fs::absolute(voice), output=wav;
                job.busy=true;
                job.done=false;
                job.thread=std::thread([&job,python,reference,output,input=std::move(input)]() {
                    std::string error;
                    if (job.worker.start(python,error))
                        job.worker.generate(reference,input,output,error);
                    else
                        job.worker.stop();
                    { std::lock_guard<std::mutex> lock(job.guard); job.error=error; }
                    job.busy=false;
                    job.done=true;
                });
            }
        }
        DrawText(short_path(status, int(w), 16).c_str(), int(x), 483, 16, job.busy ? ACCENT : SUBTLE);
        DrawText("Generated audio", int(x), 510, 17, INK);
        if (button(play_box, was_playing ? "Stop" : "Play", loaded && !job.busy)) {
            if (was_playing) StopSound(sound); else PlaySound(sound);
        }
        if (button(save_box, "Save WAV...", loaded && !job.busy)) {
            const std::string target=dialog(true);
            if (target.empty()) status="Save cancelled or zenity missing.";
            else {
                fs::copy_file(wav,target,fs::copy_options::overwrite_existing,ec);
                status=ec ? "Unable to save WAV at that location." : "WAV saved.";
            }
        }
        DrawText(short_path("All speech processing stays on your computer. No web server.", int(w), 15).c_str(),
                 int(x), height-37, 15, SUBTLE);
        EndDrawing();

    }

    if (job.worker.pid() > 0) kill(job.worker.pid(), SIGTERM);
    if (job.thread.joinable()) job.thread.join();
    job.worker.stop();
    if (loaded) UnloadSound(sound);
    CloseAudioDevice();
    CloseWindow();
    return 0;
}
