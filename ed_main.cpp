#include <notcurses/notcurses.h>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <fstream>
#include <locale>
#include <unistd.h>


#ifndef NC_RGB
#define NC_RGB(r,g,b) (((r & 0xff)<<16) | ((g & 0xff)<<8) | (b & 0xff))
#endif


template <typename T>
using vector = std::vector<T>;
using string = std::string;
using std::cout;
using std::cerr;
const string default_filename = "eddi_unnamed"; //default filename for eddi-term
const unsigned TAB_WIDTH = 4; // how many columns a tab expands to on screen

// ============================================================
// Free, stateless utility functions.
// These don't touch any Edditor instance state -- they only operate on
// whatever strings/paths are handed to them -- so they stay as plain
// functions rather than becoming members.
// ============================================================

bool file_has_data(const string& filename){ //checks if a file is empty or not
    struct stat st;
    if(stat(filename.c_str(), &st) != 0) return false;
    return st.st_size > 0;
}
bool file_exists(const string& filename){
    struct stat st;
    return stat(filename.c_str(), &st) == 0;
}

// ---- UTF-8 helpers ----
// Keep byte-offset cursors pinned to codepoint boundaries so multi-byte
// characters never get split by cursor movement, backspace, or delete.
bool utf8_is_cont(unsigned char c){ return (c & 0xC0) == 0x80; }
size_t utf8_prev(const string& s, size_t pos){
    if(pos == 0) return 0;
    pos--;
    while(pos > 0 && utf8_is_cont((unsigned char)s[pos])) pos--;
    return pos;
}



size_t utf8_next(const string& s, size_t pos){
    if(pos >= s.size()) return s.size();
    pos++;
    while(pos < s.size() && utf8_is_cont((unsigned char)s[pos])) pos++;
    return pos;
}
size_t utf8_snap(const string& s, size_t pos){
    if(pos > s.size()) pos = s.size();
    while(pos > 0 && pos < s.size() && utf8_is_cont((unsigned char)s[pos])) pos--;
    return pos;
}

// ---- Display-column helpers ----
// cursor_x is always a BYTE offset into the line (required, since we
// insert/erase into std::string by byte position). It is NOT the same
// thing as the on-screen column whenever the line contains a tab, because
// a tab is a single byte but expands to a variable number of columns.
// These two helpers are the single source of truth for converting a byte
// offset into a screen column (and for rendering a line with tabs
// expanded), so scrolling and cursor placement never desync from what's
// actually drawn on screen.
unsigned display_col(const string& line, size_t byte_pos){
    unsigned col = 0;
    size_t i = 0;
    while(i < byte_pos && i < line.size()){
        unsigned char c = (unsigned char)line[i];
        if(c == '\t'){
            col += TAB_WIDTH - (col % TAB_WIDTH);
            i++;
        } else if(!utf8_is_cont(c)){
            col++;
            i = utf8_next(line, i);
        } else {
            i++; // stray continuation byte at a bad boundary, shouldn't happen
        }
    }
    return col;
}
// Renders a line for display with tabs expanded to spaces. Used only for
// drawing -- the underlying buffer still stores the real '\t' byte, so
// saving to disk is unaffected.
string expand_tabs(const string& line){
    string out;
    unsigned col = 0;
    for(size_t i = 0; i < line.size(); i++){
        unsigned char c = (unsigned char)line[i];
        if(c == '\t'){
            unsigned spaces = TAB_WIDTH - (col % TAB_WIDTH);
            out.append(spaces, ' ');
            col += spaces;
        } else {
            out.push_back(line[i]);
            if(!utf8_is_cont(c)) col++;
        }
    }
    return out;
}
// Manual US-QWERTY shift map, used ONLY as a fallback when the terminal
// reports a Shift modifier but doesn't send a resolved shifted character
// via ni.utf8 (a real gap in some terminals). Terminals with full Kitty
// protocol support never hit this path, since their ni.utf8 is already
// layout-correct.
char shifted_char(char c){
    switch(c){
        case '1': return '!';
        case '2': return '@';
        case '3': return '#';
        case '4': return '$';
        case '5': return '%';
        case '6': return '^';
        case '7': return '&';
        case '8': return '*';
        case '9': return '(';
        case '0': return ')';
        case '-': return '_';
        case '=': return '+';
        case '[': return '{';
        case ']': return '}';
        case '\\': return '|';
        case ';': return ':';
        case '\'': return '"';
        case ',': return '<';
        case '.': return '>';
        case '/': return '?';
        case '`': return '~';
        default:
            if(c >= 'a' && c <= 'z') return (char)std::toupper((unsigned char)c);
            return c;
    }
}

// ============================================================
// Edditor -- owns all editor state and behavior.
// Every function that reads or mutates the document, cursor, scroll,
// status, rename, or notcurses handles now lives here as a member
// function instead of taking that state in as a pile of parameters.
// ============================================================
class Edditor {
private:
    // ----- Document -----
    vector<string> lines;
    string filename;
    // ----- Cursor -----
    unsigned cursor_y = 0;
    unsigned cursor_x = 0;
    // ----- Viewport -----
    unsigned scroll_y = 0;
    unsigned scroll_x = 0;
    // ----- Window -----
    unsigned rows = 0;
    unsigned cols = 0;
    // ----- Status -----
    string status_msg;
    // ----- Rename mode -----
    bool renaming = false;
    string rename_buffer;
    unsigned rename_cursor = 0;
    // ----- Piping -----
    // Set when the initial buffer content came from a pipe on stdin
    // rather than from a real file on disk. write_to_stdout additionally
    // means there is no real save target at all -- the final buffer gets
    // written to actual stdout once notcurses has released the terminal,
    // e.g. `cat draft.txt | ./eddi - > final.txt`.
    bool from_stdin = false;
    bool write_to_stdout = false;
    // ----- Notcurses -----
    notcurses* nc = nullptr;
    ncplane* stdplane = nullptr;
    // When real stdout has been redirected away from the terminal (e.g.
    // because write_to_stdout is in play), notcurses can't draw the UI on
    // stdout, so we give it the controlling terminal directly instead.
    FILE* tty_out = nullptr;

public:
    Edditor():
        filename(default_filename)
    {}
    explicit Edditor(const string& file)
        : filename(file)
    {}

    ~Edditor(){
        if(nc) notcurses_stop(nc);
        if(tty_out) fclose(tty_out);
    }

    // Full lifecycle: load/create the file, bring up notcurses, run the
    // event loop, then save on the way out.
    void run(){
        load_or_create_file();
        if(!init_notcurses()){
            cerr << "Failed to initialize Notcurses\n";
            return;
        }
        draw_buffer();
        handle_input();
        // notcurses_stop() (via the destructor path below would be too
        // late) needs to happen before we touch real stdout, so tear it
        // down explicitly here when we're about to write the buffer out.
        if(write_to_stdout){
            if(nc){ notcurses_stop(nc); nc = nullptr; }
            if(!flush_to_stdout()){
                cerr << "Warning: failed writing buffer to stdout.\n";
            }
        } else if(!save_to_disk()){
            cerr << "Warning: failed to save " << filename << " on exit.\n";
        }
    }

private:
    // ---- File I/O against this editor's own state ----
    void load_or_create_file(){
        // Piping support: either the caller explicitly asked to read
        // stdin (filename == "-"), or stdin simply isn't a terminal
        // (e.g. `cat notes.txt | ./eddi`). Either way, treat piped
        // input as the initial buffer instead of touching disk for it.
        bool explicit_stdin = (filename == "-");
        bool stdin_piped = explicit_stdin || !isatty(STDIN_FILENO);

        if(stdin_piped){
            from_stdin = true;
            // NOTE: never print to std::cout here. If write_to_stdout is
            // in play, real stdout IS the eventual save target (e.g.
            // redirected to a file by the shell) -- anything we print to
            // it before that point would land inside the saved file.
            // Diagnostics always go to stderr instead.
            cerr << "Reading piped input from stdin...\n";
            lines.clear();
            string line;
            while(std::getline(std::cin, line)) lines.push_back(line);
            if(lines.empty()) lines.push_back("");

            if(explicit_stdin){
                // "-" has no real on-disk target. Route the final saved
                // buffer to actual stdout instead, so the whole thing can
                // be chained, e.g. `cat a.txt | ./eddi - > b.txt`.
                write_to_stdout = true;
                filename = default_filename;
            }

            // We've now drained whatever was piped in. Reattach stdin to
            // the controlling terminal so notcurses can still read live
            // keyboard input for the rest of the interactive session --
            // otherwise every subsequent read would just hit EOF.
            if(!freopen("/dev/tty", "r", stdin)){
                cerr << "Warning: couldn't reopen /dev/tty for keyboard "
                        "input; interactive editing may not work.\n";
            }
            return;
        }

        if(file_has_data(filename)){
            cerr << filename << " already exists, loading...\n";
            std::ifstream in(filename);
            lines.clear();
            string line;
            while(getline(in,line)) lines.push_back(line);
            if(lines.empty()) lines.push_back("");
        } else {
            cerr << filename << " is new or empty, starting fresh...\n";
            std::ofstream initfile(filename);
            initfile.close();
            lines.clear();
            lines.push_back("");
        }
    }
    bool save_to_disk(){
        std::ofstream out(filename, std::ios::trunc);
        if(!out) return false;
        for(size_t i=0;i<lines.size();++i){
            out << lines[i];
            if(i != lines.size()-1) out << "\n";
        }
        out.flush();
        return (bool)out;
    }
    // Only used when write_to_stdout is set, and only after notcurses has
    // released the terminal -- writing here while notcurses is still
    // live would corrupt whatever's on screen.
    bool flush_to_stdout(){
        for(size_t i=0;i<lines.size();++i){
            std::cout << lines[i];
            if(i != lines.size()-1) std::cout << "\n";
        }
        std::cout.flush();
        return (bool)std::cout;
    }

    bool init_notcurses(){
        setlocale(LC_ALL,"");
        notcurses_options opts{};
        FILE* outfp = nullptr; // nullptr => notcurses defaults to stdout
        if(!isatty(STDOUT_FILENO)){
            // Real stdout has been redirected (piped to a file or another
            // command, as happens with write_to_stdout) so notcurses has
            // nothing to draw the UI on there -- give it the controlling
            // terminal directly instead.
            tty_out = fopen("/dev/tty", "w");
            if(tty_out) outfp = tty_out;
        }
        nc = notcurses_core_init(&opts, outfp);
        if(!nc) return false;
        stdplane = notcurses_stddim_yx(nc, &rows, &cols);
        notcurses_cursor_enable(nc,0,0);
        return true;
    }

    // ---- Scrolling ----
    void vertical_scroll(){
        unsigned ed_height = rows-1;
        if(cursor_y<scroll_y) scroll_y = cursor_y;
        else if(cursor_y >= scroll_y + ed_height) scroll_y = cursor_y - ed_height +1;
    }
    void horizontal_scroll(unsigned dcol){
        if(dcol < scroll_x) scroll_x = dcol;
        else if(dcol >= scroll_x + cols) scroll_x = dcol - cols + 1;
    }

    // ---- Drawing ----
    void display_status(uint32_t old_bg, uint32_t old_fg){
        ncplane_set_bg_rgb(stdplane, NC_RGB(50,50,150));
        ncplane_set_fg_rgb(stdplane, NC_RGB(200,200,200));
        if(renaming){
            ncplane_printf_yx(stdplane, rows-1, 0,
                "Rename to: %s (Enter=confirm, ESC=cancel)", rename_buffer.c_str());
        } else if(!status_msg.empty()){
            ncplane_printf_yx(stdplane, rows-1, 0, "[%s] %s", filename.c_str(), status_msg.c_str());
        } else {
            unsigned dcol = display_col(lines[cursor_y], (size_t)cursor_x);
            const char* target = write_to_stdout ? "stdout" : filename.c_str();
            ncplane_printf_yx(stdplane, rows-1, 0,
                "[%s] Line %u/%zu Col %u/%zu (ESC=quit, Ctrl+S=save, Ctrl+R=rename)",
                target,
                cursor_y+1, lines.size(),
                dcol+1, lines[cursor_y].size());
        }
        ncplane_set_bg_rgb(stdplane, old_bg);
        ncplane_set_fg_rgb(stdplane, old_fg);
    }
    void cursor_placement(unsigned ed_height, const string& current_line){
        if(renaming){
            // Cursor sits inside the "Rename to: " prompt on the status line.
            unsigned prefix_len = (unsigned)string("Rename to: ").size();
            unsigned screen_x = prefix_len + rename_cursor;
            notcurses_cursor_enable(nc, rows-1, screen_x);
        } else {
            int screen_y = (int)cursor_y - (int)scroll_y;
            // Convert the byte-offset cursor_x into an actual screen column,
            // expanding tabs as it walks the line -- this is what keeps the
            // visible cursor glued to the same spot the buffer thinks it's at.
            int screen_x = (int)display_col(current_line, (size_t)cursor_x) - (int)scroll_x;
            if(screen_y >= 0 && screen_y < (int)ed_height && screen_x >= 0 && screen_x < (int)cols){
                notcurses_cursor_enable(nc, screen_y, screen_x);
            }
        }
    }
    void display_visible_lines(unsigned ed_height, uint32_t old_fg, uint32_t old_bg){
        ncplane_set_fg_rgb(stdplane, old_fg);
        ncplane_set_bg_rgb(stdplane, old_bg);
        for(unsigned i=0;i<ed_height;i++){
            unsigned line_idx = scroll_y + i;
            if(line_idx >= lines.size()) break;
            // Expand tabs before slicing by scroll_x/cols, since scroll_x is
            // now tracked in screen columns, not raw bytes. Expanded output
            // is single-byte-per-column ASCII wherever tabs were, so the
            // substr below lines up with what horizontal_scroll computed.
            string line = expand_tabs(lines[line_idx]);
            if(line.size() > scroll_x){
                string visible = line.substr(scroll_x, cols);
                ncplane_printf_yx(stdplane, i, 0, "%s", visible.c_str());
            } else {
                ncplane_printf_yx(stdplane, i, 0, "");
            }
        }
    }
    void draw_buffer(){
        ncplane_erase(stdplane);
        unsigned ed_height = rows - 1; // last line reserved for status
        uint32_t old_bg = NC_RGB(30,30,30), old_fg = NC_RGB(200,200,200);
        //shows the visible lines of the buffer, based on scroll position and window size
        display_visible_lines(ed_height, old_fg, old_bg);
        //status line at the bottom of application
        display_status(old_bg, old_fg);
        //cursor logic: if renaming, cursor is placed in the rename prompt, otherwise it is placed in the buffer
        unsigned safe_y = (cursor_y < lines.size()) ? cursor_y : (lines.empty() ? 0 : (unsigned)lines.size()-1);
        const string& current_line = lines.empty() ? rename_buffer /* unused placeholder, renaming path ignores this */ : lines[safe_y];
        cursor_placement(ed_height, current_line);
        notcurses_render(nc);
    }

    // ---- Input handling ----
    bool handleRenameInput(const ncinput& ni, bool shift_held){
        if(ni.id == NCKEY_ENTER || ni.id == '\n'){
            string new_name = rename_buffer;
            while(!new_name.empty() &&
                  (new_name.front() == ' ' || new_name.front() == '\t'))
                new_name.erase(new_name.begin());
            while(!new_name.empty() &&
                  (new_name.back() == ' ' || new_name.back() == '\t'))
                new_name.pop_back();
            if(new_name.empty()){
                status_msg = "Rename cancelled: name cannot be empty.";
                renaming = false;
            } else if(new_name == filename){
                status_msg = "Name unchanged.";
                renaming = false;
            } else if(file_exists(new_name)){
                status_msg = "Rename failed: '" + new_name + "' already exists.";
            } else {
                if(std::rename(filename.c_str(), new_name.c_str()) == 0){
                    filename = new_name;
                    status_msg = "Renamed to '" + filename + "'.";
                    renaming = false;
                } else {
                    status_msg = string("Rename failed: ") + std::strerror(errno);
                }
            }
        }
        else if(ni.id == NCKEY_BACKSPACE || ni.id == 127){
            if(rename_cursor > 0){
                size_t start = utf8_prev(rename_buffer, rename_cursor);
                rename_buffer.erase(start, rename_cursor - start);
                rename_cursor = (unsigned)start;
            }
        }
        else if(ni.id == NCKEY_DEL){
            if(rename_cursor < rename_buffer.size()){
                size_t end = utf8_next(rename_buffer, rename_cursor);
                rename_buffer.erase(rename_cursor, end - rename_cursor);
            }
        }
        else if(ni.id == NCKEY_LEFT){
            if(rename_cursor > 0)
                rename_cursor = (unsigned)utf8_prev(rename_buffer, rename_cursor);
        }
        else if(ni.id == NCKEY_RIGHT){
            if(rename_cursor < rename_buffer.size())
                rename_cursor = (unsigned)utf8_next(rename_buffer, rename_cursor);
        }
        else if(ni.id == NCKEY_HOME){
            rename_cursor = 0;
        }
        else if(ni.id == NCKEY_END){
            rename_cursor = (unsigned)rename_buffer.size();
        }
        else if(ni.id == NCKEY_TAB || ni.id == '\t'){
            // No-op: a tab (or any other control byte) has no business in a
            // filename, and inserting one here would desync rename_cursor
            // from the visible prompt column exactly the way it used to
            // desync the main editor cursor.
        }
        else if(ni.utf8[0] != '\0' && (unsigned char)ni.utf8[0] >= 0x20){
            char base = (ni.id >= 32 && ni.id < 127) ? (char)ni.id : '\0';
            bool utf8_has_shifted =
                !(ni.utf8[1] == '\0' &&
                  base != '\0' &&
                  ni.utf8[0] == base);
            string s;
            if(utf8_has_shifted){
                s = string(ni.utf8);
            } else if(shift_held && base != '\0'){
                s = string(1, shifted_char(base));
            } else {
                s = string(ni.utf8);
            }
            rename_buffer.insert(rename_cursor, s);
            rename_cursor += (unsigned)s.size();
        }
        else if(ni.id >= 32 && ni.id < 127){
            char c = (char)ni.id;
            char to_insert = shift_held ? shifted_char(c) : c;
            rename_buffer.insert(rename_cursor, 1, to_insert);
            ++rename_cursor;
        }
        return true;
    }

    void handle_normal_input(const ncinput& ni, bool shift_held, bool ctrl_held,
                              bool is_ctrl_r, bool is_ctrl_s){
        status_msg.clear(); // any keypress dismisses a prior status message
        if(is_ctrl_r){ // enter rename mode
            if(write_to_stdout){
                // There's no real file to rename -- the buffer is headed
                // to stdout on exit no matter what name is shown.
                status_msg = "Rename disabled: output is piped to stdout.";
            } else {
                renaming = true;
                rename_buffer = filename;
                rename_cursor = (unsigned)rename_buffer.size();
            }
        } else if(ni.id == NCKEY_BACKSPACE || ni.id == 127){
            if(cursor_x>0){
                size_t start = utf8_prev(lines[cursor_y], cursor_x);
                lines[cursor_y].erase(start, cursor_x - start);
                cursor_x = (unsigned)start;
            } else if(cursor_y>0){
                cursor_x = (unsigned)lines[cursor_y-1].size();
                lines[cursor_y-1] += lines[cursor_y];
                lines.erase(lines.begin()+cursor_y);
                cursor_y--;
            }
        } else if(ni.id == NCKEY_DEL){
            if(cursor_x < lines[cursor_y].size()){
                size_t end = utf8_next(lines[cursor_y], cursor_x);
                lines[cursor_y].erase(cursor_x, end - cursor_x);
            } else if(cursor_y < lines.size()-1){
                lines[cursor_y] += lines[cursor_y+1];
                lines.erase(lines.begin()+cursor_y+1);
            }
        } else if(ni.id == NCKEY_ENTER || ni.id=='\n'){
            string rest = lines[cursor_y].substr(cursor_x);
            lines[cursor_y] = lines[cursor_y].substr(0,cursor_x);
            lines.insert(lines.begin()+cursor_y+1,rest);
            cursor_y++;
            cursor_x=0;
        } else if(ni.id == NCKEY_LEFT){
            if(cursor_x>0) cursor_x = (unsigned)utf8_prev(lines[cursor_y], cursor_x);
            else if(cursor_y>0){ cursor_y--; cursor_x = (unsigned)lines[cursor_y].size(); }
        } else if(ni.id == NCKEY_RIGHT){
            if(cursor_x<lines[cursor_y].size()) cursor_x = (unsigned)utf8_next(lines[cursor_y], cursor_x);
            else if(cursor_y<lines.size()-1){ cursor_y++; cursor_x=0; }
        } else if(ni.id == NCKEY_UP){
            if(cursor_y>0) cursor_y--;
            cursor_x = std::min(cursor_x,(unsigned)lines[cursor_y].size());
            cursor_x = (unsigned)utf8_snap(lines[cursor_y], cursor_x);
        } else if(ni.id == NCKEY_DOWN){
            if(cursor_y<lines.size()-1) cursor_y++;
            cursor_x = std::min(cursor_x,(unsigned)lines[cursor_y].size());
            cursor_x = (unsigned)utf8_snap(lines[cursor_y], cursor_x);
        } else if(ni.id == NCKEY_HOME){
            cursor_x = 0;
        } else if(ni.id == NCKEY_END){
            cursor_x = (unsigned)lines[cursor_y].size();
        } else if(ni.id == NCKEY_PGUP){
            unsigned ed_height = rows-1;
            cursor_y = (cursor_y > ed_height) ? cursor_y - ed_height : 0;
            cursor_x = std::min(cursor_x,(unsigned)lines[cursor_y].size());
            cursor_x = (unsigned)utf8_snap(lines[cursor_y], cursor_x);
        } else if(ni.id == NCKEY_PGDOWN){
            unsigned ed_height = rows-1;
            unsigned max_y = (unsigned)lines.size()-1;
            cursor_y = std::min(cursor_y + ed_height, max_y);
            cursor_x = std::min(cursor_x,(unsigned)lines[cursor_y].size());
            cursor_x = (unsigned)utf8_snap(lines[cursor_y], cursor_x);
        } else if(is_ctrl_s){ // Ctrl+S
            if(write_to_stdout){
                // Writing plain text to real stdout here, while notcurses
                // is still actively rendering to the terminal, would
                // corrupt the screen -- the buffer only gets flushed to
                // stdout once, after notcurses releases the terminal on
                // exit (see run()).
                status_msg = "Output goes to stdout on exit (Ctrl+S disabled while piping).";
            } else {
                bool ok = save_to_disk();
                status_msg = ok ? "Saved." : "SAVE FAILED (check disk space / permissions).";
            }
        } else if(ni.id == NCKEY_TAB || ni.id == '\t'){
            // Insert a real tab byte. cursor_x still just advances by 1
            // byte here -- that part was always correct, since a tab is
            // exactly one byte. What used to break was rendering/scroll
            // code treating that one byte as one *column*; those are now
            // fixed via display_col()/expand_tabs() instead of here.
            lines[cursor_y].insert(cursor_x, 1, '\t');
            cursor_x++;
        } else if(ni.utf8[0] != '\0' && (unsigned char)ni.utf8[0] >= 0x20){
            char base = (ni.id >= 32 && ni.id < 127) ? (char)ni.id : '\0';
            bool utf8_has_shifted = !(ni.utf8[1] == '\0' && base != '\0' && ni.utf8[0] == base);
            if(utf8_has_shifted){
                // Trust the terminal's resolution -- correct for the
                // user's real layout whenever the terminal supports it.
                string s(ni.utf8);
                lines[cursor_y].insert(cursor_x, s);
                cursor_x += (unsigned)s.size();
            } else if(shift_held && base != '\0'){
                // Terminal gave us Shift-as-modifier but no resolved
                // glyph. Apply the US-QWERTY fallback table.
                char to_insert = shifted_char(base);
                lines[cursor_y].insert(cursor_x,1,to_insert);
                cursor_x++;
            } else {
                string s(ni.utf8);
                lines[cursor_y].insert(cursor_x, s);
                cursor_x += (unsigned)s.size();
            }
        } else if(ni.id >= 32 && ni.id < 127){
            char c = (char)ni.id;
            char to_insert = shift_held ? shifted_char(c) : c;
            lines[cursor_y].insert(cursor_x,1,to_insert);
            cursor_x++;
        }
    }

    void handle_input(){
        ncinput ni;
        for(;;){
            uint32_t id = notcurses_get_blocking(nc, &ni);
            (void)id;
            // Kitty keyboard protocol sends both press and release events.
            // We only want to act on press/repeat, not release.
            if(ni.evtype == NCTYPE_RELEASE) continue;
            // ESC behaves differently depending on mode: cancels renaming if
            // we're in that mode, otherwise quits the editor as before.
            if(ni.id == NCKEY_ESC){
                if(renaming){
                    renaming = false;
                    status_msg = "Rename cancelled.";
                    draw_buffer();
                    continue;
                }
                break;
            }
            // Defensive bounds check against future edits introducing a bug.
            if(cursor_y >= lines.size()) cursor_y = lines.empty() ? 0 : (unsigned)lines.size()-1;
            if(lines.empty()) lines.push_back("");
            bool shift_held = (ni.modifiers & NCKEY_MOD_SHIFT) != 0;
            bool ctrl_held  = (ni.modifiers & NCKEY_MOD_CTRL) != 0;
            bool is_ctrl_s = (ni.id == 19) ||
                              (ctrl_held && (ni.id == 's' || ni.id == 'S'));
            bool is_ctrl_r = (ni.id == 18) || // legacy Ctrl+R byte code
                              (ctrl_held && (ni.id == 'r' || ni.id == 'R'));
            // ---------------- RENAME MODE ----------------
            // While renaming, keystrokes edit rename_buffer instead of the
            // document. Normal editing input is fully suspended so nothing
            // leaks through into the buffer by accident.
            if(renaming){
                handleRenameInput(ni, shift_held);
                draw_buffer();
                continue;
            }
            // ---------------- NORMAL EDITING MODE ----------------
            handle_normal_input(ni, shift_held, ctrl_held, is_ctrl_r, is_ctrl_s);
            //vertical scrolling
            vertical_scroll();
            //horiz scrolling -- scroll_x is tracked in screen COLUMNS, not
            //raw bytes, so convert cursor_x through display_col() first.
            //This is what keeps horizontal scroll (and therefore the visible
            //cursor position) glued to the right spot on lines containing
            //tabs, instead of drifting the way it used to.
            {
                unsigned dcol = display_col(lines[cursor_y], (size_t)cursor_x);
                horizontal_scroll(dcol);
            }
            draw_buffer();
        }
    }
};

int main(int argc, char* argv[]){
    string filename = argc > 1 ? argv[1] : default_filename;
    Edditor ed(filename);
    ed.run();
    return 0;
}