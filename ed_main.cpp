#include <notcurses/notcurses.h>
#include <iostream>
#include <string> 
#include <vector>
#include <algorithm>
#include <cctype>
#include <sys/stat.h>
#include <fstream>
#include <locale>
#include <cstring>

#ifndef NC_RGB
#define NC_RGB(r,g,b) (((r & 0xff)<<16) | ((g & 0xff)<<8) | (b & 0xff))
#endif

template <typename T>
using vector = std::vector<T>;
using string = std::string;
using std::cout;
using std::cerr;


string default_filename = "eddi_unnamed"; //default filename for eddi-term


bool file_has_data(const string& filename){ //checks if a file is empty or not
    struct stat st;
    if(stat(filename.c_str(), &st) != 0) return false;
    return st.st_size > 0;
}


vector<string> read_file(const string& filename){ //turns the file into a series of vectors/lines
    std::ifstream in(filename);
    vector<string> lines;
    string line;
    while(getline(in,line)) lines.push_back(line);
    if(lines.empty()) lines.push_back("");
    return lines;
}


// Returns true on success. Caller is responsible for surfacing failure
// to the user — we no longer fail silently.
bool save_file(const string& filename, const vector<string>& lines){
    std::ofstream out(filename, std::ios::trunc);
    if(!out) return false;
    for(size_t i=0;i<lines.size();++i){
        out << lines[i];
        if(i != lines.size()-1) out << "\n";
    }
    out.flush();
    return (bool)out; // false if any write/flush failed
}

// ---- UTF-8 helpers ----
// cursor_x is a BYTE offset into a std::string. These helpers keep it
// pinned to codepoint boundaries so multi-byte characters (accents,
// non-Latin scripts, etc.) never get split by cursor movement, backspace,
// or delete.
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

// Snaps a byte offset that may have landed mid-character (e.g. after
// moving between lines of different content) back to the nearest
// preceding codepoint boundary.
size_t utf8_snap(const string& s, size_t pos){
    if(pos > s.size()) pos = s.size();
    while(pos > 0 && pos < s.size() && utf8_is_cont((unsigned char)s[pos])) pos--;
    return pos;
}

// Manual US-QWERTY shift map, used ONLY as a fallback when the terminal
// reports a Shift modifier but doesn't send a resolved shifted character
// via ni.utf8 (this is a real gap in some terminals, VS Code's integrated
// terminal included). This is a deliberate compromise: it fixes the
// common case on US layouts, at the cost of being wrong on non-US
// physical keyboards for that same fallback path. Terminals with full
// Kitty protocol support (kitty, foot, wezterm, ghostty, ...) never hit
// this path at all, since their ni.utf8 is already layout-correct.
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

void draw_buffer(ncplane* ns, const vector<string>& lines,
                 unsigned cursor_y, unsigned cursor_x,
                 unsigned scroll_y, unsigned scroll_x,
                 unsigned rows, unsigned cols,
                 const string& filename, struct notcurses* nc,
                 const string& status_msg){ //draw buffer and moves the hardware cursor

    ncplane_erase(ns);
    unsigned ed_height = rows - 1; // last line reserved for status
    uint32_t old_bg = NC_RGB(30,30,30), old_fg = NC_RGB(200,200,200);

    ncplane_set_fg_rgb(ns, old_fg);
    ncplane_set_bg_rgb(ns, old_bg);

    for(unsigned i=0;i<ed_height;i++){ //controls what is visibly displayed
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

    //status line at the bottom of application
    ncplane_set_bg_rgb(ns, NC_RGB(50,50,150));
    ncplane_set_fg_rgb(ns, NC_RGB(200,200,200));
    if(!status_msg.empty()){
        ncplane_printf_yx(ns, rows-1, 0, "[%s] %s", filename.c_str(), status_msg.c_str());
    } else {
        ncplane_printf_yx(ns, rows-1, 0,
            "[%s] Line %u/%zu Col %u/%zu (ESC=quit, Ctrl+S=save)",
            filename.c_str(),
            cursor_y+1, lines.size(),
            cursor_x+1, lines[cursor_y].size());
    }

    ncplane_set_bg_rgb(ns, old_bg);
    ncplane_set_fg_rgb(ns, old_fg);

    //cursor logic
    int screen_y = cursor_y - scroll_y;
    int screen_x = cursor_x - scroll_x;
    if(screen_y >= 0 && screen_y < (int)ed_height && screen_x >= 0 && screen_x < (int)cols){
        notcurses_cursor_enable(nc, screen_y, screen_x);
    }

    notcurses_render(nc);
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
    string status_msg; // transient message (e.g. save result), overrides the hint line

    draw_buffer(stdplane, lines, cursor_y, cursor_x, scroll_y, scroll_x, rows, cols, filename, nc, status_msg);

    ncinput ni;
    for(;;){
        uint32_t id = notcurses_get_blocking(nc, &ni);

        // Kitty keyboard protocol sends both press and release events.
        // We only want to act on press/repeat, not release.
        if(ni.evtype == NCTYPE_RELEASE) continue;
        if(id == NCKEY_ESC) break;

        // Defensive bounds check: nothing should ever push cursor_y out
        // of range, but if a future edit introduces a bug, this stops
        // an out-of-bounds vector access instead of crashing/corrupting.
        if(cursor_y >= lines.size()) cursor_y = lines.empty() ? 0 : (unsigned)lines.size()-1;
        if(lines.empty()) lines.push_back("");

        bool shift_held = (ni.modifiers & NCKEY_MOD_SHIFT) != 0;
        bool ctrl_held  = (ni.modifiers & NCKEY_MOD_CTRL) != 0;

        bool is_ctrl_s = (ni.id == 19) ||
                          (ctrl_held && (ni.id == 's' || ni.id == 'S'));

        status_msg.clear(); // any keypress dismisses a prior status message

		//control handling
        if(ni.id == NCKEY_BACKSPACE || ni.id == 127){
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
                // glyph (VS Code's terminal today). Apply the US-QWERTY
                // fallback table.
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

        //vertical scrolling
        unsigned ed_height = rows-1;
        if(cursor_y<scroll_y) scroll_y = cursor_y;
        else if(cursor_y >= scroll_y + ed_height) scroll_y = cursor_y - ed_height +1;

        //horiz scrolling
        if(cursor_x < scroll_x) scroll_x = cursor_x;
        else if(cursor_x >= scroll_x + cols) scroll_x = cursor_x - cols +1;

        draw_buffer(stdplane, lines, cursor_y, cursor_x, scroll_y, scroll_x, rows, cols, filename, nc, status_msg);
    }

    if(!save_file(filename, lines)){
        cerr << "Warning: failed to save " << filename << " on exit.\n";
    }
    notcurses_stop(nc);
    return 0;
}