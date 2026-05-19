#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>

#define MAX_PLAYERS 3
#define HISTORY_MAX 64
#define SAVE_PATH EXT_PATH("apps_data/nineball_score/history.txt")

typedef enum {
    ScreenMode,
    ScreenName,
    ScreenGame,
    ScreenHistory,
} Screen;

typedef enum {
    EventFoul = 1,       // 犯规：本家-1，上家+1
    EventNormal = 4,     // 普胜
    EventSmallGold = 7,  // 小金
    EventBigGold = 10,   // 大金
} ScoreEvent;

typedef struct {
    char name[4];
    int score;
    int wins;
} Player;

typedef struct {
    char line[48];
} HistoryLine;

typedef struct {
    Player p[MAX_PLAYERS];
    uint8_t player_count;
    uint8_t order[MAX_PLAYERS];
    uint8_t current_slot; // 左到右顺序里的当前本家位置
    uint16_t rack_no;
    Screen screen;
    uint8_t edit_player;
    uint8_t edit_char;
    HistoryLine history[HISTORY_MAX];
    uint8_t history_count;
    uint8_t history_page;
    NotificationApp* notifications;
    FuriMutex* mutex;
} NineballApp;

static void buzz(NineballApp* app) {
    notification_message(app->notifications, &sequence_single_vibro);
}

static uint8_t prev_slot(NineballApp* app) {
    if(app->current_slot == 0) return app->player_count - 1;
    return app->current_slot - 1;
}

static void add_history(NineballApp* app, const char* text) {
    if(app->history_count < HISTORY_MAX) {
        snprintf(app->history[app->history_count].line, sizeof(app->history[0].line), "%s", text);
        app->history_count++;
    } else {
        for(uint8_t i = 1; i < HISTORY_MAX; i++) app->history[i - 1] = app->history[i];
        snprintf(app->history[HISTORY_MAX - 1].line, sizeof(app->history[0].line), "%s", text);
    }
}

static void save_history(NineballApp* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, EXT_PATH("apps_data/nineball_score"));
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, SAVE_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        char buf[96];
        int len = snprintf(buf, sizeof(buf), "9球追分 历史记录\n局数:%u\n", app->rack_no);
        storage_file_write(file, buf, len);
        for(uint8_t i = 0; i < app->history_count; i++) {
            len = snprintf(buf, sizeof(buf), "%s\n", app->history[i].line);
            storage_file_write(file, buf, len);
        }
        storage_file_close(file);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

static void reorder_three(NineballApp* app, uint8_t winner_id, uint8_t loser_id) {
    if(app->player_count != 3) return;
    uint8_t other = 0;
    for(uint8_t i = 0; i < 3; i++) {
        if(i != winner_id && i != loser_id) other = i;
    }
    app->order[0] = winner_id;
    app->order[1] = loser_id;
    app->order[2] = other;
    app->current_slot = 0;
}

static void apply_event(NineballApp* app, ScoreEvent ev) {
    uint8_t cur_id = app->order[app->current_slot];
    uint8_t up_id = app->order[prev_slot(app)];
    int pts = (int)ev;
    char line[48];

    if(ev == EventFoul) {
        // 犯规：本家犯规，本家扣 1 给上家
        app->p[cur_id].score -= 1;
        app->p[up_id].score += 1;
        app->p[up_id].wins++;
        app->rack_no++;
        snprintf(line, sizeof(line), "%03u %s犯规 %s+1", app->rack_no, app->p[cur_id].name, app->p[up_id].name);
        add_history(app, line);
        if(app->player_count == 3) reorder_three(app, up_id, cur_id);
    } else {
        // 普胜/小金/大金/让球得分：本家得分，上家扣分
        app->p[cur_id].score += pts;
        app->p[up_id].score -= pts;
        app->p[cur_id].wins++;
        app->rack_no++;
        const char* label = ev == EventNormal ? "普胜" : (ev == EventSmallGold ? "小金" : "大金");
        snprintf(line, sizeof(line), "%03u %s%s +%d", app->rack_no, app->p[cur_id].name, label, pts);
        add_history(app, line);
        if(app->player_count == 3) reorder_three(app, cur_id, up_id);
    }
    save_history(app);
    buzz(app);
}

static void reset_game(NineballApp* app) {
    for(uint8_t i = 0; i < MAX_PLAYERS; i++) {
        app->p[i].score = 0;
        app->p[i].wins = 0;
        app->order[i] = i;
    }
    app->current_slot = 0;
    app->rack_no = 0;
    app->history_count = 0;
    add_history(app, "新比赛开始");
    save_history(app);
    buzz(app);
}

static void draw_table(Canvas* canvas) {
    canvas_draw_rbox(canvas, 2, 12, 124, 47, 6);
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_rbox(canvas, 6, 16, 116, 39, 4);
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_circle(canvas, 10, 20, 2);
    canvas_draw_circle(canvas, 118, 20, 2);
    canvas_draw_circle(canvas, 10, 51, 2);
    canvas_draw_circle(canvas, 118, 51, 2);
}

static void draw_callback(Canvas* canvas, void* ctx) {
    NineballApp* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);

    if(app->screen == ScreenMode) {
        canvas_draw_str(canvas, 28, 10, "9球追分");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 8, 26, "选择模式");
        canvas_draw_str(canvas, 20, 44, app->player_count == 2 ? "> 两人模式" : "  两人模式");
        canvas_draw_str(canvas, 20, 56, app->player_count == 3 ? "> 三人模式" : "  三人模式");
        canvas_draw_str(canvas, 78, 64, "OK开始");
    } else if(app->screen == ScreenName) {
        canvas_draw_str(canvas, 25, 10, "修改名字");
        canvas_set_font(canvas, FontSecondary);
        for(uint8_t i = 0; i < app->player_count; i++) {
            char buf[24];
            snprintf(buf, sizeof(buf), "%c P%d: %s", i == app->edit_player ? '>' : ' ', i + 1, app->p[i].name);
            canvas_draw_str(canvas, 22, 26 + i * 12, buf);
        }
        canvas_draw_str(canvas, 4, 63, "上下改字母 左右换位 OK下一步");
    } else if(app->screen == ScreenHistory) {
        canvas_draw_str(canvas, 35, 10, "历史记录");
        canvas_set_font(canvas, FontSecondary);
        uint8_t start = app->history_page * 5;
        for(uint8_t i = 0; i < 5; i++) {
            uint8_t idx = start + i;
            if(idx < app->history_count) canvas_draw_str(canvas, 2, 22 + i * 9, app->history[idx].line);
        }
        canvas_draw_str(canvas, 2, 64, "上下翻页 Back返回");
    } else {
        draw_table(canvas);
        char top[40];
        snprintf(top, sizeof(top), "第%u局  OK换人  长OK历史", app->rack_no + 1);
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 3, 9, top);

        uint8_t col_w = app->player_count == 3 ? 41 : 60;
        for(uint8_t slot = 0; slot < app->player_count; slot++) {
            uint8_t id = app->order[slot];
            uint8_t x = 5 + slot * col_w;
            if(slot == app->current_slot) canvas_draw_frame(canvas, x - 2, 18, col_w - 2, 36);
            canvas_draw_str(canvas, x, 27, app->p[id].name);
            char score[12];
            snprintf(score, sizeof(score), "%d", app->p[id].score);
            canvas_set_font(canvas, FontBigNumbers);
            canvas_draw_str(canvas, x + 4, 48, score);
            canvas_set_font(canvas, FontSecondary);
            char wins[10];
            snprintf(wins, sizeof(wins), "W:%d", app->p[id].wins);
            canvas_draw_str(canvas, x, 58, wins);
        }
        canvas_draw_str(canvas, 3, 64, "↑犯规 ←大金 ↓小金 →普胜");
    }
    furi_mutex_release(app->mutex);
}

static void input_callback(InputEvent* input, void* ctx) {
    FuriMessageQueue* queue = ctx;
    furi_message_queue_put(queue, input, FuriWaitForever);
}

int32_t nineball_score_app(void* p) {
    UNUSED(p);
    NineballApp* app = malloc(sizeof(NineballApp));
    memset(app, 0, sizeof(NineballApp));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    app->player_count = 2;
    app->screen = ScreenMode;
    snprintf(app->p[0].name, sizeof(app->p[0].name), "AAA");
    snprintf(app->p[1].name, sizeof(app->p[1].name), "BBB");
    snprintf(app->p[2].name, sizeof(app->p[2].name), "CCC");
    reset_game(app);

    FuriMessageQueue* event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, draw_callback, app);
    view_port_input_callback_set(view_port, input_callback, event_queue);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    bool running = true;
    InputEvent input;
    while(running) {
        if(furi_message_queue_get(event_queue, &input, FuriWaitForever) == FuriStatusOk) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            if(input.type == InputTypeShort) {
                if(app->screen == ScreenMode) {
                    if(input.key == InputKeyLeft || input.key == InputKeyRight || input.key == InputKeyUp || input.key == InputKeyDown) app->player_count = app->player_count == 2 ? 3 : 2;
                    else if(input.key == InputKeyOk) { reset_game(app); app->screen = ScreenName; }
                    else if(input.key == InputKeyBack) running = false;
                } else if(app->screen == ScreenName) {
                    char* n = app->p[app->edit_player].name;
                    if(input.key == InputKeyLeft) app->edit_char = (app->edit_char + 2) % 3;
                    else if(input.key == InputKeyRight) app->edit_char = (app->edit_char + 1) % 3;
                    else if(input.key == InputKeyUp) n[app->edit_char] = n[app->edit_char] >= 'Z' ? 'A' : n[app->edit_char] + 1;
                    else if(input.key == InputKeyDown) n[app->edit_char] = n[app->edit_char] <= 'A' ? 'Z' : n[app->edit_char] - 1;
                    else if(input.key == InputKeyOk) { app->edit_player++; app->edit_char = 0; if(app->edit_player >= app->player_count) app->screen = ScreenGame; }
                    else if(input.key == InputKeyBack) app->screen = ScreenMode;
                } else if(app->screen == ScreenHistory) {
                    if(input.key == InputKeyUp && app->history_page > 0) app->history_page--;
                    else if(input.key == InputKeyDown && (app->history_page + 1) * 5 < app->history_count) app->history_page++;
                    else if(input.key == InputKeyBack) app->screen = ScreenGame;
                } else {
                    if(input.key == InputKeyUp) apply_event(app, EventFoul);
                    else if(input.key == InputKeyRight) apply_event(app, EventNormal);
                    else if(input.key == InputKeyDown) apply_event(app, EventSmallGold);
                    else if(input.key == InputKeyLeft) apply_event(app, EventBigGold);
                    else if(input.key == InputKeyOk) app->current_slot = (app->current_slot + 1) % app->player_count;
                    else if(input.key == InputKeyBack) app->screen = ScreenMode;
                }
            } else if(input.type == InputTypeLong) {
                if(app->screen == ScreenGame && input.key == InputKeyBack) reset_game(app);
                else if(app->screen == ScreenGame && input.key == InputKeyOk) { app->history_page = 0; app->screen = ScreenHistory; }
            }
            furi_mutex_release(app->mutex);
            view_port_update(view_port);
        }
    }

    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_message_queue_free(event_queue);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    furi_mutex_free(app->mutex);
    free(app);
    return 0;
}
