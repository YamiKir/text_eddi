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

#ifndef NC_RGB
#define NC_RGB(r,g,b) (((r & 0xff)<<16) | ((g & 0xff)<<8) | (b & 0xff))
#endif

template <typename T>
using vector = std::vector<T>;
using string = std::string;
using std::cout;
using std::cerr;


const string default_filename = "eddi_unnamed"; //default filename for eddi-term

void vertical_scroll(unsigned cursor_y, unsigned& scroll_y, int rows){
    unsigned ed_height = rows-1;
    if(cursor_y<scroll_y) scroll_y = cursor_y;
    else if(cursor_y >= scroll_y + ed_height) scroll_y = cursor_y - ed_height +1;
}
void horizontal_scroll(unsigned cursor_x, unsigned& scroll_x, int cols){
    if(cursor_x < scroll_x) scroll_x = cursor_x;
        else if(cursor_x >= scroll_x + cols) scroll_x = cursor_x - cols +1;
}

bool file_has_data(const string& filename){ //checks if a file is empty or not
    struct stat st;
    if(stat(filename.c_str(), &st) != 0) return false;
    return st.st_size > 0;
}

bool file_exists(const string& filename){
    struct stat st;
    return stat(filename.c_str(), &st) == 0;
}


vector<string> read_file(const string& filename){ //turns the file into a series of vectors/lines
    std::ifstream in(filename);
    vector<string> lines;
    string line;
    while(getline(in,line)) lines.push_back(line);
    if(lines.empty()) lines.push_back("");
    return lines;
}


bool save_file(const string& filename, const vector<string>& lines){
    std::ofstream out(filename, std::ios::trunc);
    if(!out) return false;
    for(size_t i=0;i<lines.size();++i){
        out << lines[i];
        if(i != lines.size()-1) out << "\n";
    }
    out.flush();
    return (bool)out;
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

void display_status(ncplane* ns, unsigned rows, unsigned cols,int cursor_y, int cursor_x, const vector<string>& lines,uint32_t old_bg, uint32_t old_fg,
                    const string& filename, const string& status_msg,
                    bool renaming, const string& rename_buffer){
    ncplane_set_bg_rgb(ns, NC_RGB(50,50,150));
    ncplane_set_fg_rgb(ns, NC_RGB(200,200,200));
    if(renaming){
        ncplane_printf_yx(ns, rows-1, 0,
            "Rename to: %s (Enter=confirm, ESC=cancel)", rename_buffer.c_str());
    } else if(!status_msg.empty()){
        ncplane_printf_yx(ns, rows-1, 0, "[%s] %s", filename.c_str(), status_msg.c_str());
    } else {
        ncplane_printf_yx(ns, rows-1, 0,
            "[%s] Line %u/%zu Col %u/%zu (ESC=quit, Ctrl+S=save, Ctrl+R=rename)",
            filename.c_str(),
            cursor_y+1, lines.size(),
            cursor_x+1, lines[cursor_y].size());
    }

    ncplane_set_bg_rgb(ns, old_bg);
    ncplane_set_fg_rgb(ns, old_fg);

}
void cursor_placement(ncplane* ns, unsigned cursor_y, unsigned cursor_x, bool renaming, unsigned rename_cursor, unsigned rows, unsigned cols, struct notcurses* nc,int ed_height, int scroll_y, int scroll_x){
    if(renaming){
        // Cursor sits inside the "Rename to: " prompt on the status line.
        unsigned prefix_len = (unsigned)string("Rename to: ").size();
        unsigned screen_x = prefix_len + rename_cursor;
        notcurses_cursor_enable(nc, rows-1, screen_x);
    } else {
        int screen_y = cursor_y - scroll_y;
        int screen_x = cursor_x - scroll_x;
        if(screen_y >= 0 && screen_y < (int)ed_height && screen_x >= 0 && screen_x < (int)cols){
            notcurses_cursor_enable(nc, screen_y, screen_x);
        }
    }
}
void display_visible_lines(ncplane* ns, const vector<string>& lines, unsigned scroll_y, unsigned scroll_x, unsigned ed_height, unsigned cols, uint32_t old_fg, uint32_t old_bg){
    ncplane_set_fg_rgb(ns, old_fg);
    ncplane_set_bg_rgb(ns, old_bg);

    for(unsigned i=0;i<ed_height;i++){
        unsigned line_idx = scroll_y + i;
        if(line_idx >= lines.size()) break;
        string line = lines[line_idx];
        if(line.size() > scroll_x){
            string visible = line.substr(scroll_x, cols);
            ncplane_printf_yx(ns, i, 0, "%s", visible.c_str());
        } else {
            ncplane_printf_yx(ns, i, 0, "");
        }
    }

}
void draw_buffer(ncplane* ns, const vector<string>& lines,
                 unsigned cursor_y, unsigned cursor_x,
                 unsigned scroll_y, unsigned scroll_x,
                 unsigned rows, unsigned cols,
                 const string& filename, struct notcurses* nc,
                 const string& status_msg,
                 bool renaming, const string& rename_buffer, unsigned rename_cursor){

    ncplane_erase(ns);
    unsigned ed_height = rows - 1; // last line reserved for status
    uint32_t old_bg = NC_RGB(30,30,30), old_fg = NC_RGB(200,200,200);

    //shows the visible lines of the buffer, based on scroll position and window size
    display_visible_lines(ns, lines, scroll_y, scroll_x, ed_height, cols, old_fg, old_bg);

    //status line at the bottom of application
    display_status(ns, rows, cols, cursor_y, cursor_x, lines, old_bg, old_fg,
                   filename, status_msg, renaming, rename_buffer);

    //cursor logic if renaming, cursor is placed in the rename prompt, otherwise it is placed in the buffer
    
    cursor_placement(ns, cursor_y, cursor_x, renaming, rename_cursor, rows, cols, nc, ed_height, scroll_y, scroll_x);

    notcurses_render(nc);
}


bool handleRenameInput(
    const ncinput& ni,
    bool shift_held,
    bool& renaming,
    std::string& rename_buffer,
    unsigned& rename_cursor,
    std::string& filename,
    std::string& status_msg
){
    if(ni.id == NCKEY_ENTER || ni.id == '\n'){
        std::string new_name = rename_buffer;

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
                status_msg =
                    std::string("Rename failed: ") + std::strerror(errno);
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
    else if(ni.utf8[0] != '\0'){
        char base = (ni.id >= 32 && ni.id < 127)
                        ? (char)ni.id
                        : '\0';

        bool utf8_has_shifted =
            !(ni.utf8[1] == '\0' &&
              base != '\0' &&
              ni.utf8[0] == base);

        std::string s;

        if(utf8_has_shifted){
            s = std::string(ni.utf8);
        } else if(shift_held && base != '\0'){
            s = std::string(1, shifted_char(base));
        } else {
            s = std::string(ni.utf8);
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
void handle_normal_input(const ncinput& ni, int rows, int cols, bool shift_held, bool ctrl_held,
                         vector<string>& lines,
                         unsigned& cursor_y, unsigned& cursor_x,
                         string& status_msg, const string& filename, bool& is_ctrl_r, bool& is_ctrl_s, bool& renaming, string& rename_buffer, unsigned& rename_cursor){
    status_msg.clear(); // any keypress dismisses a prior status message

        if(is_ctrl_r){ // enter rename mode
            renaming = true;
            rename_buffer = filename;
            rename_cursor = (unsigned)rename_buffer.size();
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
            bool ok = save_file(filename, lines);
            status_msg = ok ? "Saved." : "SAVE FAILED (check disk space / permissions).";
        } else if(ni.utf8[0] != '\0'){
            char base = (ni.id >= 32 && ni.id < 127) ? (char)ni.id : '\0';
            bool utf8_has_shifted = !(ni.utf8[1] == '\0' && base != '\0' && ni.utf8[0] == base);

            if(utf8_has_shifted){
                // Trust the terminal's resolution — correct for the
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

void handle_input(struct notcurses* nc, ncplane* stdplane, vector<string>& lines,
                  unsigned& cursor_y, unsigned& cursor_x,
                  unsigned& scroll_y, unsigned& scroll_x,
                  unsigned rows, unsigned cols,
                  string& filename, string& status_msg,
                  bool& renaming, string& rename_buffer, unsigned& rename_cursor){

                    ncinput ni;
    for(;;){
        uint32_t id = notcurses_get_blocking(nc, &ni);

        // Kitty keyboard protocol sends both press and release events.
        // We only want to act on press/repeat, not release.
        if(ni.evtype == NCTYPE_RELEASE) continue;

        // ESC behaves differently depending on mode: cancels renaming if
        // we're in that mode, otherwise quits the editor as before.
        
        if(id == NCKEY_ESC){
            if(renaming){
                renaming = false;
                status_msg = "Rename cancelled.";
                draw_buffer(stdplane, lines, cursor_y, cursor_x, scroll_y, scroll_x, rows, cols,
                            filename, nc, status_msg, renaming, rename_buffer, rename_cursor);
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
    handleRenameInput(
        ni,
        shift_held,
        renaming,
        rename_buffer,
        rename_cursor,
        filename,
        status_msg
    );

    draw_buffer(stdplane, lines,
                cursor_y, cursor_x,
                scroll_y, scroll_x,
                rows, cols,
                filename, nc,
                status_msg,
                renaming,
                rename_buffer,
                rename_cursor);

    continue;
}

        // ---------------- NORMAL EDITING MODE ----------------
        handle_normal_input(
            ni,
            rows, cols,
            shift_held,
            ctrl_held,
            lines,
            cursor_y,
            cursor_x,
            status_msg,
            filename,
            is_ctrl_r,
            is_ctrl_s,
            renaming,
            rename_buffer,
            rename_cursor
        );

        //vertical scrolling
       vertical_scroll(cursor_y, scroll_y, rows);

        //horiz scrolling
        horizontal_scroll(cursor_x, scroll_x, cols);

        draw_buffer(stdplane, lines, cursor_y, cursor_x, scroll_y, scroll_x, rows, cols,
                    filename, nc, status_msg, renaming, rename_buffer, rename_cursor);
    }


                  }
int main(int argc, char* argv[]){
    string filename = argc > 1 ? argv[1] : default_filename;
    vector<string> lines;

    if(file_has_data(filename)){
        cout << filename << " already exists, loading...\n";
        lines = read_file(filename);
    } else {
        cout << filename << " is new or empty, starting fresh...\n";
        std::ofstream initfile(filename);
        initfile.close();
        lines.push_back("");
    }

    setlocale(LC_ALL,"");
    notcurses_options opts{};
    struct notcurses* nc = notcurses_core_init(&opts, nullptr);
    if(!nc){ cerr << "Failed to initialize Notcurses\n"; return 1; }

    unsigned rows, cols;
    ncplane* stdplane = notcurses_stddim_yx(nc, &rows, &cols);

    notcurses_cursor_enable(nc,0,0);
    unsigned cursor_y=0, cursor_x=0;
    unsigned scroll_y=0, scroll_x=0;
    string status_msg; // transient message (save result, rename result, errors)

    // Rename-mode state
    bool renaming = false;
    string rename_buffer;
    unsigned rename_cursor = 0;

    draw_buffer(stdplane, lines, cursor_y, cursor_x, scroll_y, scroll_x, rows, cols,
                filename, nc, status_msg, renaming, rename_buffer, rename_cursor);

    // Main event loop
    handle_input(nc, stdplane, lines, cursor_y, cursor_x, scroll_y, scroll_x,
                 rows, cols, filename, status_msg,
                 renaming, rename_buffer, rename_cursor);

    if(!save_file(filename, lines)){
        cerr << "Warning: failed to save " << filename << " on exit.\n";
    }
    notcurses_stop(nc);
    return 0;
}