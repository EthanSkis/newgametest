#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <conio.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

static const int MAP_W     = 78;
static const int MAP_H     = 20;
static const int VISION_R  = 6;
static const int CHASE_R   = 8;
static const int XP_PER_LV = 50;
static const int MAX_MSG   = 3;

enum Tile    { T_VOID, T_WALL, T_FLOOR, T_HALL, T_STAIR };
enum ItemK   { I_HEAL, I_ATK, I_DEF };
enum Key     { K_NONE, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_USE, K_QUIT };

struct Vec { int x, y; };

struct Mob {
    std::string name;
    Vec pos;
    int hp, maxHp, atk, def, xpVal;
    char gl;
    int col;
    bool alive;
};

struct Item {
    std::string name;
    ItemK kind;
    int value;
    Vec pos;
    char gl;
    int col;
    bool onMap;
};

struct Rect {
    int x, y, w, h;
    int cx() const { return x + w / 2; }
    int cy() const { return y + h / 2; }
    bool overlaps(const Rect& o, int pad = 2) const {
        return x - pad < o.x + o.w && x + w + pad > o.x &&
               y - pad < o.y + o.h && y + h + pad > o.y;
    }
};

struct State {
    Tile grid[MAP_H][MAP_W];
    int  vis[MAP_H][MAP_W];
    std::vector<Rect> rooms;
    Mob player;
    std::vector<Mob> mobs;
    std::vector<Item> mapItems;
    std::vector<Item> bag;
    std::vector<std::string> log;
    int floor, xp, lvl, score;
    bool running, won;
};

// ── Terminal ──────────────────────────────────────────────

#ifdef _WIN32

static HANDLE hOut, hIn;
static DWORD origOutMode, origInMode;

static void rawOff() {
    SetConsoleMode(hOut, origOutMode);
    SetConsoleMode(hIn, origInMode);
    printf("\033[?25h\033[0m\033[2J\033[H");
    fflush(stdout);
}

static void rawOn() {
    hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    hIn  = GetStdHandle(STD_INPUT_HANDLE);
    GetConsoleMode(hOut, &origOutMode);
    GetConsoleMode(hIn, &origInMode);
    atexit(rawOff);
    SetConsoleMode(hOut, origOutMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN);
    SetConsoleMode(hIn, ENABLE_PROCESSED_INPUT);
    printf("\033[?25l");
    fflush(stdout);
}

static Key readKey() {
    int c = _getch();
    switch (c) {
        case 'q': case 'Q': return K_QUIT;
        case 'e': case 'E': return K_USE;
        case 'w': case 'W': return K_UP;
        case 's': case 'S': return K_DOWN;
        case 'a': case 'A': return K_LEFT;
        case 'd': case 'D': return K_RIGHT;
        case 0: case 0xE0: {
            int ext = _getch();
            switch (ext) {
                case 72: return K_UP;
                case 80: return K_DOWN;
                case 75: return K_LEFT;
                case 77: return K_RIGHT;
            }
            return K_NONE;
        }
    }
    return K_NONE;
}

static void waitKey() { _getch(); }

static void writeOut(const std::string& buf) {
    DWORD written;
    WriteConsoleA(hOut, buf.c_str(), (DWORD)buf.size(), &written, NULL);
}

#else

static struct termios saved_termios;

static void rawOff() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios);
    printf("\033[?25h\033[0m\033[2J\033[H");
    fflush(stdout);
}

static void rawOn() {
    tcgetattr(STDIN_FILENO, &saved_termios);
    atexit(rawOff);
    struct termios t = saved_termios;
    t.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    t.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    t.c_cflag |= CS8;
    t.c_cc[VMIN]  = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
    printf("\033[?25l");
    fflush(stdout);
}

static Key readKey() {
    char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return K_NONE;
    switch (c) {
        case 'q': case 'Q': return K_QUIT;
        case 'e': case 'E': return K_USE;
        case 'w': case 'W': return K_UP;
        case 's': case 'S': return K_DOWN;
        case 'a': case 'A': return K_LEFT;
        case 'd': case 'D': return K_RIGHT;
        case '\033': {
            char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) != 1) return K_NONE;
            if (read(STDIN_FILENO, &seq[1], 1) != 1) return K_NONE;
            if (seq[0] == '[') {
                if (seq[1] == 'A') return K_UP;
                if (seq[1] == 'B') return K_DOWN;
                if (seq[1] == 'C') return K_RIGHT;
                if (seq[1] == 'D') return K_LEFT;
            }
            return K_NONE;
        }
    }
    return K_NONE;
}

static void waitKey() {
    char c;
    if (read(STDIN_FILENO, &c, 1) < 0) return;
}

static void writeOut(const std::string& buf) {
    if (write(STDOUT_FILENO, buf.c_str(), buf.size()) < 0) return;
}

#endif

// ── Helpers ───────────────────────────────────────────────

static void msg(State& s, const std::string& m) {
    s.log.push_back(m);
    if ((int)s.log.size() > MAX_MSG)
        s.log.erase(s.log.begin());
}

static bool walkable(Tile t) {
    return t == T_FLOOR || t == T_HALL || t == T_STAIR;
}

static int dist2(int x1, int y1, int x2, int y2) {
    int dx = x2 - x1, dy = y2 - y1;
    return dx * dx + dy * dy;
}

// ── Dungeon Generation ───────────────────────────────────

static void genDungeon(State& s) {
    memset(s.grid, 0, sizeof(s.grid));
    memset(s.vis,  0, sizeof(s.vis));
    s.rooms.clear();
    s.mobs.clear();
    s.mapItems.clear();

    int want = 6 + rand() % 4;
    for (int tries = 0; tries < 400 && (int)s.rooms.size() < want; tries++) {
        Rect r;
        r.w = 5 + rand() % 8;
        r.h = 4 + rand() % 5;
        r.x = 1 + rand() % (MAP_W - r.w - 2);
        r.y = 1 + rand() % (MAP_H - r.h - 2);
        bool ok = true;
        for (auto& o : s.rooms)
            if (r.overlaps(o)) { ok = false; break; }
        if (!ok) continue;
        for (int y = r.y; y < r.y + r.h; y++)
            for (int x = r.x; x < r.x + r.w; x++)
                s.grid[y][x] = T_FLOOR;
        s.rooms.push_back(r);
    }

    if (s.rooms.size() < 2) {
        Rect r = {2, 2, 10, 6};
        for (int y = r.y; y < r.y + r.h; y++)
            for (int x = r.x; x < r.x + r.w; x++)
                s.grid[y][x] = T_FLOOR;
        s.rooms.push_back(r);
        Rect r2 = {20, 2, 10, 6};
        for (int y = r2.y; y < r2.y + r2.h; y++)
            for (int x = r2.x; x < r2.x + r2.w; x++)
                s.grid[y][x] = T_FLOOR;
        s.rooms.push_back(r2);
    }

    for (int i = 1; i < (int)s.rooms.size(); i++) {
        int x1 = s.rooms[i - 1].cx(), y1 = s.rooms[i - 1].cy();
        int x2 = s.rooms[i].cx(),     y2 = s.rooms[i].cy();
        auto carve = [&](int cy, int cx) {
            if (cy >= 0 && cy < MAP_H && cx >= 0 && cx < MAP_W)
                if (s.grid[cy][cx] != T_FLOOR)
                    s.grid[cy][cx] = T_HALL;
        };
        if (rand() % 2) {
            for (int x = std::min(x1, x2); x <= std::max(x1, x2); x++) carve(y1, x);
            for (int y = std::min(y1, y2); y <= std::max(y1, y2); y++) carve(y, x2);
        } else {
            for (int y = std::min(y1, y2); y <= std::max(y1, y2); y++) carve(y, x1);
            for (int x = std::min(x1, x2); x <= std::max(x1, x2); x++) carve(y2, x);
        }
    }

    for (int y = 0; y < MAP_H; y++)
        for (int x = 0; x < MAP_W; x++)
            if (s.grid[y][x] == T_FLOOR || s.grid[y][x] == T_HALL)
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int ny = y + dy, nx = x + dx;
                        if (ny >= 0 && ny < MAP_H && nx >= 0 && nx < MAP_W)
                            if (s.grid[ny][nx] == T_VOID)
                                s.grid[ny][nx] = T_WALL;
                    }

    s.player.pos = {s.rooms[0].cx(), s.rooms[0].cy()};

    if (s.floor < 5) {
        auto& last = s.rooms.back();
        s.grid[last.cy()][last.cx()] = T_STAIR;
    }

    int ne = 3 + s.floor * 2;
    int roomCount = (int)s.rooms.size();
    for (int i = 0; i < ne; i++) {
        int ri = 1 + rand() % (roomCount - 1);
        auto& rm = s.rooms[ri];
        int ew = std::max(1, rm.w - 2);
        int eh = std::max(1, rm.h - 2);
        int ex = rm.x + 1 + rand() % ew;
        int ey = rm.y + 1 + rand() % eh;
        if (ex == s.player.pos.x && ey == s.player.pos.y) continue;

        Mob m;
        int fl = s.floor;
        if (fl >= 5 && i == 0)
            m = {"Dragon",  {ex, ey}, 60,       60,       15,     8, 100, 'D', 196, true};
        else {
            int roll = rand() % 100;
            if (roll < 30)
                m = {"Rat",      {ex, ey}, 5+fl,    5+fl,    2+fl,   0, 10,  'r', 130, true};
            else if (roll < 55)
                m = {"Goblin",   {ex, ey}, 8+fl*2,  8+fl*2,  4+fl,   1, 20,  'g', 34,  true};
            else if (roll < 80)
                m = {"Skeleton", {ex, ey}, 12+fl*2, 12+fl*2, 5+fl,   2, 30,  's', 255, true};
            else
                m = {"Orc",      {ex, ey}, 18+fl*3, 18+fl*3, 7+fl,   4, 45,  'o', 160, true};
        }
        s.mobs.push_back(m);
    }

    int ni = 2 + rand() % 3;
    for (int i = 0; i < ni; i++) {
        int ri = rand() % roomCount;
        auto& rm = s.rooms[ri];
        int iw = std::max(1, rm.w - 2);
        int ih = std::max(1, rm.h - 2);
        int ix = rm.x + 1 + rand() % iw;
        int iy = rm.y + 1 + rand() % ih;
        Item it;
        int fl = s.floor;
        int roll = rand() % 3;
        if (roll == 0)
            it = {"Health Potion",  I_HEAL, 15 + fl * 5, {ix, iy}, '!', 201, true};
        else if (roll == 1)
            it = {"Attack Scroll",  I_ATK,  2,           {ix, iy}, '+', 39,  true};
        else
            it = {"Defense Amulet", I_DEF,  1,           {ix, iy}, '*', 220, true};
        s.mapItems.push_back(it);
    }

    msg(s, "You enter floor " + std::to_string(s.floor) + ".");
    if (s.floor == 5)
        msg(s, "A fearsome Dragon lurks on this floor!");
}

// ── Visibility ────────────────────────────────────────────

static void updateVis(State& s) {
    for (int y = 0; y < MAP_H; y++)
        for (int x = 0; x < MAP_W; x++)
            if (s.vis[y][x] == 2) s.vis[y][x] = 1;

    int px = s.player.pos.x, py = s.player.pos.y;
    int r2 = VISION_R * VISION_R;
    int ylo = std::max(0, py - VISION_R), yhi = std::min(MAP_H - 1, py + VISION_R);
    int xlo = std::max(0, px - VISION_R), xhi = std::min(MAP_W - 1, px + VISION_R);
    for (int y = ylo; y <= yhi; y++)
        for (int x = xlo; x <= xhi; x++)
            if (dist2(px, py, x, y) <= r2)
                s.vis[y][x] = 2;
}

// ── Combat ────────────────────────────────────────────────

static void doAttack(State& s, Mob& atk, Mob& def) {
    int dmg = std::max(1, atk.atk - def.def);
    def.hp -= dmg;

    msg(s, atk.name + " hits " + def.name + " for " + std::to_string(dmg) + "!");

    if (def.hp <= 0) {
        def.alive = false;
        if (&def != &s.player) {
            s.xp    += def.xpVal;
            s.score += def.xpVal;
            msg(s, def.name + " slain! +" + std::to_string(def.xpVal) + " XP");
            while (s.xp >= s.lvl * XP_PER_LV) {
                s.xp -= s.lvl * XP_PER_LV;
                s.lvl++;
                s.player.maxHp += 10;
                s.player.hp = std::min(s.player.hp + 10, s.player.maxHp);
                s.player.atk += 2;
                s.player.def += 1;
                msg(s, "LEVEL UP! Now level " + std::to_string(s.lvl) + "!");
            }
            if (def.name == "Dragon" && s.floor == 5)
                s.won = true;
        }
    }
}

// ── Player Turn ───────────────────────────────────────────

static void movePlayer(State& s, int dx, int dy) {
    int nx = s.player.pos.x + dx;
    int ny = s.player.pos.y + dy;
    if (nx < 0 || nx >= MAP_W || ny < 0 || ny >= MAP_H) return;
    if (!walkable(s.grid[ny][nx])) return;

    for (auto& m : s.mobs)
        if (m.alive && m.pos.x == nx && m.pos.y == ny) {
            doAttack(s, s.player, m);
            return;
        }

    s.player.pos = {nx, ny};

    for (auto& it : s.mapItems)
        if (it.onMap && it.pos.x == nx && it.pos.y == ny) {
            it.onMap = false;
            s.bag.push_back(it);
            msg(s, "Picked up " + it.name + ".");
        }

    if (s.grid[ny][nx] == T_STAIR) {
        s.floor++;
        s.score += 50;
        genDungeon(s);
    }
}

// ── Enemy AI ──────────────────────────────────────────────

static void moveMobs(State& s) {
    int px = s.player.pos.x, py = s.player.pos.y;
    for (auto& m : s.mobs) {
        if (!m.alive) continue;
        int d = dist2(m.pos.x, m.pos.y, px, py);
        int dx = 0, dy = 0;

        if (d <= CHASE_R * CHASE_R) {
            if (m.pos.x < px) dx = 1;  else if (m.pos.x > px) dx = -1;
            if (m.pos.y < py) dy = 1;  else if (m.pos.y > py) dy = -1;
            if (dx && dy) { if (rand() % 2) dx = 0; else dy = 0; }
        } else {
            int r = rand() % 5;
            if (r == 0) dx = 1;  else if (r == 1) dx = -1;
            else if (r == 2) dy = 1; else if (r == 3) dy = -1;
        }

        int nx = m.pos.x + dx, ny = m.pos.y + dy;
        if (nx < 0 || nx >= MAP_W || ny < 0 || ny >= MAP_H) continue;
        if (!walkable(s.grid[ny][nx])) continue;

        if (nx == px && ny == py) {
            doAttack(s, m, s.player);
            continue;
        }

        bool blocked = false;
        for (auto& o : s.mobs)
            if (&o != &m && o.alive && o.pos.x == nx && o.pos.y == ny)
                { blocked = true; break; }
        if (!blocked) m.pos = {nx, ny};
    }
}

// ── Use Item ──────────────────────────────────────────────

static void useItem(State& s) {
    if (s.bag.empty()) { msg(s, "No items!"); return; }
    auto it = s.bag.back();
    s.bag.pop_back();
    switch (it.kind) {
        case I_HEAL:
            s.player.hp = std::min(s.player.hp + it.value, s.player.maxHp);
            msg(s, "Used " + it.name + ". +" + std::to_string(it.value) + " HP");
            break;
        case I_ATK:
            s.player.atk += it.value;
            msg(s, "Used " + it.name + ". +" + std::to_string(it.value) + " ATK");
            break;
        case I_DEF:
            s.player.def += it.value;
            msg(s, "Used " + it.name + ". +" + std::to_string(it.value) + " DEF");
            break;
    }
}

// ── Renderer ──────────────────────────────────────────────

static void render(State& s) {
    std::string buf;
    buf.reserve(10000);
    buf += "\033[H";

    for (int y = 0; y < MAP_H; y++) {
        for (int x = 0; x < MAP_W; x++) {
            int v = s.vis[y][x];
            if (v == 0) { buf += ' '; continue; }

            bool lit = (v == 2);

            if (lit && s.player.pos.x == x && s.player.pos.y == y) {
                buf += "\033[1;38;5;46m@\033[0m";
                continue;
            }

            if (lit) {
                bool drawn = false;
                for (auto& m : s.mobs)
                    if (m.alive && m.pos.x == x && m.pos.y == y) {
                        char tmp[40];
                        snprintf(tmp, sizeof(tmp), "\033[1;38;5;%dm%c\033[0m", m.col, m.gl);
                        buf += tmp;
                        drawn = true;
                        break;
                    }
                if (drawn) continue;

                for (auto& it : s.mapItems)
                    if (it.onMap && it.pos.x == x && it.pos.y == y) {
                        char tmp[40];
                        snprintf(tmp, sizeof(tmp), "\033[1;38;5;%dm%c\033[0m", it.col, it.gl);
                        buf += tmp;
                        drawn = true;
                        break;
                    }
                if (drawn) continue;
            }

            Tile t = s.grid[y][x];
            if (lit) {
                switch (t) {
                    case T_WALL:  buf += "\033[38;5;245m#\033[0m"; break;
                    case T_FLOOR: buf += "\033[38;5;240m.\033[0m"; break;
                    case T_HALL:  buf += "\033[38;5;94m#\033[0m";  break;
                    case T_STAIR: buf += "\033[1;38;5;226m>\033[0m"; break;
                    default:      buf += ' '; break;
                }
            } else {
                switch (t) {
                    case T_WALL:  buf += "\033[38;5;236m#\033[0m"; break;
                    case T_FLOOR: case T_HALL:
                                  buf += "\033[38;5;236m.\033[0m"; break;
                    case T_STAIR: buf += "\033[38;5;236m>\033[0m"; break;
                    default:      buf += ' '; break;
                }
            }
        }
        buf += "\033[K\n";
    }

    for (int i = 0; i < MAX_MSG; i++) {
        if (i < (int)s.log.size())
            buf += "\033[38;5;228m " + s.log[i] + "\033[0m";
        buf += "\033[K\n";
    }

    char hud[256];
    snprintf(hud, sizeof(hud),
        "\033[1;38;5;255m HP:\033[38;5;%dm%d/%d"
        "\033[38;5;255m | ATK:\033[38;5;208m%d"
        "\033[38;5;255m | DEF:\033[38;5;75m%d"
        "\033[38;5;255m | LV:\033[38;5;228m%d"
        "\033[38;5;255m | XP:\033[38;5;228m%d/%d"
        "\033[38;5;255m | FL:\033[38;5;226m%d"
        "\033[38;5;255m | BAG:\033[38;5;201m%d"
        "\033[38;5;255m | [E]Use [Q]Quit\033[0m",
        s.player.hp > s.player.maxHp / 3 ? 46 : 196,
        s.player.hp, s.player.maxHp,
        s.player.atk, s.player.def,
        s.lvl, s.xp, s.lvl * XP_PER_LV,
        s.floor, (int)s.bag.size());
    buf += hud;
    buf += "\033[K";

    writeOut(buf);
}

// ── Screens ───────────────────────────────────────────────

static void titleScreen() {
    printf("\033[2J\033[H\n");
    printf("\033[1;38;5;196m");
    printf("        ___                                    ___                    _\n");
    printf("       |   \\ _  _ _ _  __ _ ___ ___ _ _      / __|_ _ __ ___ __ __ | |___ _ _\n");
    printf("       | |) | || | ' \\/ _` / -_) _ \\ ' \\    | (__| '_/ _` \\ V  V / | / -_) '_|\n");
    printf("       |___/ \\_,_|_||_\\__, \\___\\___/_||_|    \\___|_| \\__,_|\\_/\\_/  |_\\___|_|\n");
    printf("                      |___/\n");
    printf("\033[0m\n");
    printf("  \033[38;5;228mDescend 5 floors of a dangerous dungeon and slay the Dragon to win!\033[0m\n\n");
    printf("  \033[1;38;5;255mControls:\033[0m\n");
    printf("    \033[38;5;46mWASD\033[0m / \033[38;5;46mArrow Keys\033[0m  Move & attack (bump into enemies)\n");
    printf("    \033[38;5;46mE\033[0m                   Use item from bag\n");
    printf("    \033[38;5;46mQ\033[0m                   Quit\n\n");
    printf("  \033[1;38;5;255mLegend:\033[0m\n");
    printf("    \033[1;38;5;46m@\033[0m You   ");
    printf("\033[1;38;5;130mr\033[0m Rat   ");
    printf("\033[1;38;5;34mg\033[0m Goblin   ");
    printf("\033[1;38;5;255ms\033[0m Skeleton   ");
    printf("\033[1;38;5;160mo\033[0m Orc   ");
    printf("\033[1;38;5;196mD\033[0m Dragon\n");
    printf("    \033[1;38;5;201m!\033[0m Potion ");
    printf("\033[1;38;5;39m+\033[0m Scroll ");
    printf("\033[1;38;5;220m*\033[0m Amulet  ");
    printf("\033[1;38;5;226m>\033[0m Stairs\n\n");
    printf("  \033[38;5;240mPress any key to begin...\033[0m\n");
    fflush(stdout);
    waitKey();
}

static void endScreen(const State& s) {
    printf("\033[2J\033[H\n\n");
    if (s.won) {
        printf("  \033[1;38;5;226m");
        printf("  +=========================================+\n");
        printf("  |              V I C T O R Y              |\n");
        printf("  |     The Dragon has been vanquished!     |\n");
        printf("  +=========================================+\n");
        printf("  \033[0m\n\n");
    } else {
        printf("  \033[1;38;5;196m");
        printf("  +=========================================+\n");
        printf("  |            G A M E   O V E R            |\n");
        printf("  |         You have perished...            |\n");
        printf("  +=========================================+\n");
        printf("  \033[0m\n\n");
    }
    printf("  \033[38;5;255mFinal Score: \033[1;38;5;228m%d\033[0m\n", s.score);
    printf("  \033[38;5;255mLevel:       \033[38;5;228m%d\033[0m\n", s.lvl);
    printf("  \033[38;5;255mFloor:       \033[38;5;228m%d\033[0m\n\n", s.floor);
    printf("  \033[38;5;240mPress any key to exit...\033[0m\n");
    fflush(stdout);
    waitKey();
}

// ── Main ──────────────────────────────────────────────────

int main() {
    srand((unsigned)time(nullptr));
    rawOn();
    titleScreen();

    State s{};
    s.player  = {"Hero", {0, 0}, 30, 30, 5, 2, 0, '@', 46, true};
    s.floor   = 1;
    s.xp      = 0;
    s.lvl     = 1;
    s.score   = 0;
    s.running = true;
    s.won     = false;

    genDungeon(s);
    printf("\033[2J");

    bool quit = false;
    while (!quit && s.player.alive && !s.won) {
        updateVis(s);
        render(s);

        Key k = readKey();
        switch (k) {
            case K_UP:    movePlayer(s,  0, -1); break;
            case K_DOWN:  movePlayer(s,  0,  1); break;
            case K_LEFT:  movePlayer(s, -1,  0); break;
            case K_RIGHT: movePlayer(s,  1,  0); break;
            case K_USE:   useItem(s);            break;
            case K_QUIT:  quit = true;           break;
            default: continue;
        }

        if (!quit && s.player.alive && !s.won)
            moveMobs(s);

        if (s.player.hp <= 0)
            s.player.alive = false;
    }

    if (!quit)
        endScreen(s);

    return 0;
}
