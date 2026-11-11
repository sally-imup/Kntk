/*
 * touchwm - minimal touch-first X11 window manager for the Termux VM desktop.
 *
 *  - Every normal window is maximized to the work area (no title bars).
 *  - Bottom taskbar with large touch targets:
 *      APPS button      tap: open/close the app menu
 *      window button    tap: focus it; tap again while focused: minimize
 *                       tap a minimized window: restore it
 *                       long-press (~0.6 s): close it
 *  - App menu is read from ~/.config/touchwm/apps ("Label|command" per line).
 *    The built-in "Log out" entry stops the window manager.
 *  - Fullscreen windows (_NET_WM_STATE_FULLSCREEN) cover the whole screen and hide the taskbar.
 *  - Dialog / utility / transient windows keep their size and are centered.
 *  - Focus follows touch (EnterNotify); nothing depends on hover.
 *
 * Build: cc -O2 -Wall -Wextra -o touchwm touchwm.c -lX11
 */
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>

#define TASKBAR_H     72
#define APPS_W        168
#define BTN_GAP       8
#define BTN_MIN_W     150
#define BTN_MAX_W     300
#define MENU_W        420
#define MENU_ITEM_H   80
#define MENU_PAD      8
#define MAX_APPS      16
#define LONG_PRESS_MS 600

enum { A_WM_PROTOCOLS, A_WM_DELETE, A_NET_WM_STATE, A_NET_WM_STATE_FULLSCREEN, N_ATOMS };
static const char *atom_names[N_ATOMS] = {
    "WM_PROTOCOLS", "WM_DELETE_WINDOW", "_NET_WM_STATE", "_NET_WM_STATE_FULLSCREEN"
};

typedef struct Client {
    Window w;
    char name[256];
    int hidden;         /* minimized by us */
    int fullscreen;
    int dialog;         /* keep requested size, centered */
    int req_w, req_h;
    int pending_unmap;  /* UnmapWindow calls we made that must not count as withdrawal */
    struct Client *next;
} Client;

typedef struct { char label[64]; char cmd[256]; } App;

static Display *dpy;
static int scr;
static Window root, taskbar, menu;
static int sw, sh;
static Client *clients;
static Client *focused;
static int menu_open;
static App apps[MAX_APPS];
static int napps;
static GC gc;
static XFontStruct *font;
static unsigned long c_bar, c_btn, c_focus, c_hidden, c_text, c_menu;
static Atom A[N_ATOMS];
static int running = 1;
static int wm_starting = 1, other_wm;
static Time press_t;
static int press_x;

/* ---------- helpers ---------- */

static long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

static unsigned long pix(unsigned int rgb) {
    XColor c;
    c.red = ((rgb >> 16) & 255) * 257;
    c.green = ((rgb >> 8) & 255) * 257;
    c.blue = (rgb & 255) * 257;
    c.flags = DoRed | DoGreen | DoBlue;
    XAllocColor(dpy, DefaultColormap(dpy, scr), &c);
    return c.pixel;
}

static int x_error(Display *d, XErrorEvent *e) {
    (void)d;
    if (wm_starting && e->error_code == BadAccess)
        other_wm = 1;
    return 0; /* windows vanish all the time; ignore BadWindow & friends */
}

static unsigned char *get_prop(Window w, Atom prop, Atom type, unsigned long *n) {
    Atom real;
    int fmt;
    unsigned long after;
    unsigned char *data = NULL;
    *n = 0;
    if (XGetWindowProperty(dpy, w, prop, 0, 1024, False, type, &real, &fmt, n, &after, &data) != Success)
        return NULL;
    return data;
}

static int has_state(Window w, Atom state) {
    unsigned long n;
    Atom *list = (Atom *)get_prop(w, A[A_NET_WM_STATE], XA_ATOM, &n);
    int found = 0;
    for (unsigned long i = 0; list && i < n; i++)
        if (list[i] == state) found = 1;
    if (list) XFree(list);
    return found;
}

static void set_state_fullscreen(Client *c, int on) {
    c->fullscreen = on;
    if (on) {
        Atom a = A[A_NET_WM_STATE_FULLSCREEN];
        XChangeProperty(dpy, c->w, A[A_NET_WM_STATE], XA_ATOM, 32, PropModeReplace, (unsigned char *)&a, 1);
    } else {
        XDeleteProperty(dpy, c->w, A[A_NET_WM_STATE]);
    }
}

static void read_name(Client *c) {
    char *n = NULL;
    if (XFetchName(dpy, c->w, &n) && n) {
        snprintf(c->name, sizeof c->name, "%s", n);
        XFree(n);
    } else if (!c->name[0]) {
        snprintf(c->name, sizeof c->name, "Window");
    }
}

/* Returns 0 = normal tiled, 1 = dialog-like, 2 = unmanaged (docks, menus, desktop). */
enum { KIND_NORMAL, KIND_DIALOG, KIND_UNMANAGED };

static int classify(Window w) {
    int kind = KIND_NORMAL;
    Window transient = None;
    unsigned long n;
    if (XGetTransientForHint(dpy, w, &transient) && transient != None)
        kind = KIND_DIALOG;

    Atom type_prop = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    Atom *types = (Atom *)get_prop(w, type_prop, XA_ATOM, &n);
    for (unsigned long i = 0; types && i < n && kind != KIND_UNMANAGED; i++) {
        char *nm = XGetAtomName(dpy, types[i]);
        if (!nm) continue;
        if (!strcmp(nm, "_NET_WM_WINDOW_TYPE_DIALOG") || !strcmp(nm, "_NET_WM_WINDOW_TYPE_UTILITY") ||
            !strcmp(nm, "_NET_WM_WINDOW_TYPE_SPLASH"))
            kind = KIND_DIALOG;
        else if (!strcmp(nm, "_NET_WM_WINDOW_TYPE_DOCK") || !strcmp(nm, "_NET_WM_WINDOW_TYPE_DESKTOP") ||
                 !strcmp(nm, "_NET_WM_WINDOW_TYPE_MENU") || !strcmp(nm, "_NET_WM_WINDOW_TYPE_POPUP_MENU") ||
                 !strcmp(nm, "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU") || !strcmp(nm, "_NET_WM_WINDOW_TYPE_TOOLTIP") ||
                 !strcmp(nm, "_NET_WM_WINDOW_TYPE_NOTIFICATION") || !strcmp(nm, "_NET_WM_WINDOW_TYPE_COMBO") ||
                 !strcmp(nm, "_NET_WM_WINDOW_TYPE_DND"))
            kind = KIND_UNMANAGED;
        XFree(nm);
    }
    if (types) XFree(types);
    return kind;
}

static Client *find_client(Window w) {
    for (Client *c = clients; c; c = c->next)
        if (c->w == w) return c;
    return NULL;
}

static Client *first_visible(void) {
    for (Client *c = clients; c; c = c->next)
        if (!c->hidden) return c;
    return NULL;
}

/* ---------- drawing ---------- */

static void draw_label(Window win, int x, int y, int w, int h, unsigned long bg, const char *label, int left) {
    char buf[256];
    int len;
    XSetForeground(dpy, gc, bg);
    XFillRectangle(dpy, win, gc, x, y, w, h);
    if (!label || !*label) return;
    snprintf(buf, sizeof buf, "%s", label);
    len = (int)strlen(buf);
    while (len > 0 && XTextWidth(font, buf, len) > w - 24) len--;
    buf[len] = '\0';
    int tw = XTextWidth(font, buf, len);
    int tx = left ? x + 20 : x + (w - tw) / 2;
    int ty = y + (h + font->ascent - font->descent) / 2;
    XSetFont(dpy, gc, font->fid);
    XSetForeground(dpy, gc, c_text);
    XDrawString(dpy, win, gc, tx, ty, buf, len);
}

static int button_w(void) {
    int n = 0;
    for (Client *c = clients; c; c = c->next) n++;
    if (n == 0) return BTN_MAX_W;
    int avail = sw - APPS_W - 2 * BTN_GAP - BTN_GAP * (n - 1);
    int w = avail / n;
    if (w > BTN_MAX_W) w = BTN_MAX_W;
    if (w < BTN_MIN_W) w = BTN_MIN_W;
    return w;
}

static void draw_taskbar(void) {
    int bw = button_w(), i = 0;
    XSetForeground(dpy, gc, c_bar);
    XFillRectangle(dpy, taskbar, gc, 0, 0, sw, TASKBAR_H);
    draw_label(taskbar, 0, 0, APPS_W, TASKBAR_H, menu_open ? c_focus : c_btn, "APPS", 0);
    for (Client *c = clients; c; c = c->next, i++) {
        int bx = APPS_W + BTN_GAP + i * (bw + BTN_GAP);
        if (bx + bw > sw) break;
        unsigned long bg = c->hidden ? c_hidden : (c == focused ? c_focus : c_btn);
        draw_label(taskbar, bx, 6, bw, TASKBAR_H - 12, bg, c->name, 0);
    }
    XFlush(dpy);
}

static void draw_menu(void) {
    XSetForeground(dpy, gc, c_menu);
    XFillRectangle(dpy, menu, gc, 0, 0, MENU_W, MENU_PAD * 2 + napps * MENU_ITEM_H);
    for (int i = 0; i < napps; i++)
        draw_label(menu, MENU_PAD, MENU_PAD + i * MENU_ITEM_H, MENU_W - 2 * MENU_PAD, MENU_ITEM_H - MENU_PAD,
                   c_btn, apps[i].label, 1);
    XFlush(dpy);
}

/* ---------- layout & focus ---------- */

static void place(Client *c, int taskbar_on) {
    int area_h = taskbar_on ? sh - TASKBAR_H : sh;
    if (c->fullscreen) {
        XMoveResizeWindow(dpy, c->w, 0, 0, sw, sh);
    } else if (c->dialog) {
        int dw = c->req_w < sw ? c->req_w : sw;
        int dh = c->req_h < area_h ? c->req_h : area_h;
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        XMoveResizeWindow(dpy, c->w, (sw - dw) / 2, (area_h - dh) / 2, dw, dh);
    } else {
        XMoveResizeWindow(dpy, c->w, 0, 0, sw, area_h);
    }
}

static void relayout(void) {
    int taskbar_on = !(focused && focused->fullscreen && !focused->hidden);
    for (Client *c = clients; c; c = c->next)
        if (!c->hidden) place(c, taskbar_on);

    if (!taskbar_on) menu_open = 0;
    if (taskbar_on) XMapRaised(dpy, taskbar); else XUnmapWindow(dpy, taskbar);

    if (menu_open && napps > 0) {
        int mh = MENU_PAD * 2 + napps * MENU_ITEM_H;
        XMoveResizeWindow(dpy, menu, MENU_PAD, sh - TASKBAR_H - mh, MENU_W, mh);
        XMapRaised(dpy, menu);
        draw_menu();
    } else {
        XUnmapWindow(dpy, menu);
    }
    draw_taskbar();
}

static void set_focus(Client *c) {
    focused = (c && !c->hidden) ? c : NULL;
    menu_open = 0;
    if (focused) {
        XRaiseWindow(dpy, focused->w);
        XSetInputFocus(dpy, focused->w, RevertToPointerRoot, CurrentTime);
    } else {
        XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
    }
    relayout();
}

static void minimize(Client *c) {
    c->hidden = 1;
    c->pending_unmap++;
    XUnmapWindow(dpy, c->w);
    set_focus(first_visible());
}

static void restore(Client *c) {
    c->hidden = 0;
    XMapWindow(dpy, c->w);
    set_focus(c);
}

static void close_client(Client *c) {
    Atom *protos = NULL;
    unsigned long n = 0;
    int has_delete = 0;
    protos = (Atom *)get_prop(c->w, A[A_WM_PROTOCOLS], XA_ATOM, &n);
    for (unsigned long i = 0; protos && i < n; i++)
        if (protos[i] == A[A_WM_DELETE]) has_delete = 1;
    if (protos) XFree(protos);

    if (has_delete) {
        XEvent e;
        memset(&e, 0, sizeof e);
        e.xclient.type = ClientMessage;
        e.xclient.window = c->w;
        e.xclient.message_type = A[A_WM_PROTOCOLS];
        e.xclient.format = 32;
        e.xclient.data.l[0] = (long)A[A_WM_DELETE];
        e.xclient.data.l[1] = CurrentTime;
        XSendEvent(dpy, c->w, False, NoEventMask, &e);
    } else {
        XKillClient(dpy, c->w);
    }
}

static void unmanage(Client *c) {
    Client **pp = &clients;
    while (*pp && *pp != c) pp = &(*pp)->next;
    if (*pp) *pp = c->next;
    if (focused == c) focused = NULL;
    free(c);
    if (!focused) focused = first_visible();
    relayout();
}

static void manage(Window w) {
    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, w, &wa) || wa.override_redirect) return;
    int kind = classify(w);
    if (kind == KIND_UNMANAGED) {
        XMapWindow(dpy, w);
        return;
    }
    Client *c = calloc(1, sizeof *c);
    if (!c) return;
    c->w = w;
    c->dialog = (kind == KIND_DIALOG);
    c->req_w = wa.width;
    c->req_h = wa.height;
    c->fullscreen = has_state(w, A[A_NET_WM_STATE_FULLSCREEN]);
    read_name(c);
    /* Structure events for clients come through the root (SubstructureNotify) only;
     * selecting StructureNotify here too would deliver every Unmap/Destroy twice. */
    XSelectInput(dpy, w, EnterWindowMask | PropertyChangeMask);

    /* append, so the taskbar keeps windows in the order they were opened */
    Client **pp = &clients;
    while (*pp) pp = &(*pp)->next;
    *pp = c;

    XMapWindow(dpy, w);
    set_focus(c);
}

/* ---------- menu ---------- */

static void load_apps(void) {
    const char *home = getenv("HOME");
    char path[512];
    FILE *f = NULL;
    if (home) {
        snprintf(path, sizeof path, "%s/.config/touchwm/apps", home);
        f = fopen(path, "r");
    }
    if (f) {
        char line[384];
        while (napps < MAX_APPS && fgets(line, sizeof line, f)) {
            char *bar = strchr(line, '|');
            if (!bar) continue;
            *bar = '\0';
            char *cmd = bar + 1;
            cmd[strcspn(cmd, "\r\n")] = '\0';
            snprintf(apps[napps].label, sizeof apps[napps].label, "%.63s", line);
            snprintf(apps[napps].cmd, sizeof apps[napps].cmd, "%s", cmd);
            napps++;
        }
        fclose(f);
    }
    if (napps == 0) {
        const char *defaults[][2] = {
            {"Terminal", "xterm -fa Monospace -fs 16"},
            {"Browser", "firefox || chromium"},
            {"Files", "pcmanfm"},
        };
        for (unsigned i = 0; i < sizeof defaults / sizeof defaults[0]; i++) {
            snprintf(apps[napps].label, sizeof apps[napps].label, "%s", defaults[i][0]);
            snprintf(apps[napps].cmd, sizeof apps[napps].cmd, "%s", defaults[i][1]);
            napps++;
        }
    }
    if (napps < MAX_APPS) {
        snprintf(apps[napps].label, sizeof apps[napps].label, "%s", "Log out");
        snprintf(apps[napps].cmd, sizeof apps[napps].cmd, "%s", "__exit__");
        napps++;
    }
}

static void launch(const char *cmd) {
    if (fork() == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}

/* ---------- event handling ---------- */

static void on_button_press_taskbar(XButtonEvent *e) {
    press_t = (Time)now_ms();
    press_x = e->x;
}

static void on_button_release_taskbar(XButtonEvent *e) {
    long held = now_ms() - (long)press_t;
    (void)e;
    if (press_x < APPS_W) {
        if (held < LONG_PRESS_MS) {
            menu_open = !menu_open;
            relayout();
        }
        return;
    }
    int bw = button_w(), i = 0;
    Client *hit = NULL;
    for (Client *c = clients; c; c = c->next, i++) {
        int bx = APPS_W + BTN_GAP + i * (bw + BTN_GAP);
        if (bx + bw > sw) break;
        if (press_x >= bx && press_x < bx + bw) { hit = c; break; }
    }
    if (!hit) return;
    if (held >= LONG_PRESS_MS) close_client(hit);
    else if (hit->hidden) restore(hit);
    else if (hit == focused) minimize(hit);
    else set_focus(hit);
}

static void on_button_release_menu(XButtonEvent *e) {
    int idx = (e->y - MENU_PAD) / MENU_ITEM_H;
    if (e->y < MENU_PAD || idx < 0 || idx >= napps) return;
    menu_open = 0;
    relayout();
    if (!strcmp(apps[idx].cmd, "__exit__")) running = 0;
    else launch(apps[idx].cmd);
}

static void handle_client_message(XClientMessageEvent *e) {
    if (e->message_type == A[A_NET_WM_STATE]) {
        Client *c = find_client(e->window);
        if (!c || e->data.l[1] != (long)A[A_NET_WM_STATE_FULLSCREEN]) return;
        int action = (int)e->data.l[0]; /* 0 remove, 1 add, 2 toggle */
        int on = action == 1 ? 1 : action == 0 ? 0 : !c->fullscreen;
        set_state_fullscreen(c, on);
        relayout();
    }
}

static void handle_event(XEvent *ev) {
    switch (ev->type) {
    case MapRequest: {
        Window w = ev->xmaprequest.window;
        Client *c = find_client(w);
        if (!c) manage(w);
        else if (!c->hidden) XMapWindow(dpy, w);
        break;
    }
    case ConfigureRequest: {
        XConfigureRequestEvent *e = &ev->xconfigurerequest;
        if (find_client(e->window)) break; /* we decide managed geometry */
        XWindowChanges wc;
        wc.x = e->x; wc.y = e->y; wc.width = e->width; wc.height = e->height;
        wc.border_width = e->border_width; wc.sibling = e->above; wc.stack_mode = e->detail;
        XConfigureWindow(dpy, e->window, e->value_mask, &wc);
        break;
    }
    case UnmapNotify: {
        Client *c = find_client(ev->xunmap.window);
        if (!c) break;
        if (c->pending_unmap > 0) { c->pending_unmap--; break; }
        unmanage(c); /* client withdrew its window */
        break;
    }
    case DestroyNotify: {
        Client *c = find_client(ev->xdestroywindow.window);
        if (c) unmanage(c);
        break;
    }
    case EnterNotify: {
        Client *c = find_client(ev->xcrossing.window);
        if (c && ev->xcrossing.mode == NotifyNormal && c != focused && !c->hidden) set_focus(c);
        break;
    }
    case PropertyNotify: {
        Client *c = find_client(ev->xproperty.window);
        if (c && (ev->xproperty.atom == XA_WM_NAME || ev->xproperty.atom == XInternAtom(dpy, "_NET_WM_NAME", False))) {
            read_name(c);
            draw_taskbar();
        }
        break;
    }
    case Expose:
        if (ev->xexpose.count != 0) break;
        if (ev->xexpose.window == taskbar) draw_taskbar();
        else if (ev->xexpose.window == menu) draw_menu();
        break;
    case ButtonPress:
        if (ev->xbutton.window == taskbar) on_button_press_taskbar(&ev->xbutton);
        else if (ev->xbutton.window == root && menu_open) { menu_open = 0; relayout(); }
        break;
    case ButtonRelease:
        if (ev->xbutton.window == taskbar) on_button_release_taskbar(&ev->xbutton);
        else if (ev->xbutton.window == menu) on_button_release_menu(&ev->xbutton);
        break;
    case ClientMessage:
        handle_client_message(&ev->xclient);
        break;
    default:
        break;
    }
}

/* ---------- startup ---------- */

static void adopt_existing(void) {
    Window r, p, *kids = NULL;
    unsigned int n = 0;
    if (!XQueryTree(dpy, root, &r, &p, &kids, &n)) return;
    for (unsigned int i = 0; i < n; i++) {
        XWindowAttributes wa;
        if (kids[i] == taskbar || kids[i] == menu) continue;
        if (XGetWindowAttributes(dpy, kids[i], &wa) && wa.map_state == IsViewable && !wa.override_redirect)
            manage(kids[i]);
    }
    if (kids) XFree(kids);
}

int main(void) {
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "touchwm: cannot open display\n");
        return 1;
    }
    signal(SIGCHLD, SIG_IGN); /* auto-reap launched apps */
    XSetErrorHandler(x_error);

    scr = DefaultScreen(dpy);
    root = RootWindow(dpy, scr);
    sw = DisplayWidth(dpy, scr);
    sh = DisplayHeight(dpy, scr);

    XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask | ButtonPressMask);
    XSync(dpy, False);
    if (other_wm) {
        fprintf(stderr, "touchwm: another window manager is already running\n");
        return 1;
    }
    wm_starting = 0;

    for (int i = 0; i < N_ATOMS; i++) A[i] = XInternAtom(dpy, atom_names[i], False);

    c_bar = pix(0x12161c);
    c_btn = pix(0x2c3440);
    c_focus = pix(0x2f7de1);
    c_hidden = pix(0x1d232b);
    c_text = pix(0xffffff);
    c_menu = pix(0x1a2029);

    font = XLoadQueryFont(dpy, "-*-*-bold-r-normal--26-*-*-*-*-*-iso8859-1");
    if (!font) font = XLoadQueryFont(dpy, "-*-*-medium-r-normal--22-*-*-*-*-*-iso8859-1");
    if (!font) font = XLoadQueryFont(dpy, "fixed");
    if (!font) {
        fprintf(stderr, "touchwm: no usable font (install xfonts-base)\n");
        return 1;
    }

    XSetWindowAttributes swa;
    swa.override_redirect = True;
    swa.background_pixel = c_bar;
    swa.event_mask = ButtonPressMask | ButtonReleaseMask | ExposureMask;
    taskbar = XCreateWindow(dpy, root, 0, sh - TASKBAR_H, sw, TASKBAR_H, 0, CopyFromParent, InputOutput,
                            CopyFromParent, CWOverrideRedirect | CWBackPixel | CWEventMask, &swa);

    load_apps();
    swa.background_pixel = c_menu;
    menu = XCreateWindow(dpy, root, MENU_PAD, sh - TASKBAR_H - 100, MENU_W, 100, 0, CopyFromParent, InputOutput,
                         CopyFromParent, CWOverrideRedirect | CWBackPixel | CWEventMask, &swa);

    gc = XCreateGC(dpy, root, 0, NULL);
    XSetWindowBackground(dpy, root, pix(0x1b2430));
    XClearWindow(dpy, root);

    adopt_existing();
    relayout();
    XFlush(dpy);

    while (running) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        handle_event(&ev);
        XFlush(dpy);
    }

    XCloseDisplay(dpy);
    return 0;
}
