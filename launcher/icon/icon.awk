# Pixel-art app icon: a CRT showing a four-pane window, an arcade stick in front.
# awk -f icon.awk > icon.ppm   (32x32, P3), then scale with nearest neighbour.
function rect(x0, y0, x1, y1, c,   x, y) { for (y = y0; y <= y1; y++) for (x = x0; x <= x1; x++) px[x, y] = c }
function disc(cx, cy, r, c,   x, y) {
    for (y = int(cy - r - 1); y <= cy + r + 1; y++)
        for (x = int(cx - r - 1); x <= cx + r + 1; x++)
            if ((x - cx) ^ 2 + (y - cy) ^ 2 <= r * r) px[x, y] = c
}
# a round 3x3 arcade button: corners left as they were, a highlight top-left
function button(x, y, c, cl) { # a button seen from above at an angle: a 4x2 oval
    rect(x + 1, y, x + 2, y, cl); rect(x, y + 1, x + 3, y + 1, c); px[x + 1, y] = cl; px[x + 2, y] = c
}
BEGIN {
    N = 32
    # palette
    col["bg"] = "27 30 62"; col["bg2"] = "36 40 82"; col["K"] = "12 12 22"
    col["case"] = "214 207 190"; col["caseD"] = "150 144 130"; col["caseL"] = "240 236 224"
    col["scr"] = "14 30 40"; col["scan"] = "20 44 58"; col["glow"] = "40 80 96"
    col["p1"] = "255 70 160"; col["p2"] = "60 220 255"; col["p3"] = "255 214 70"; col["p4"] = "150 100 255"; col["triL"] = "255 248 200"
    col["ball"] = "232 52 60"; col["ballD"] = "160 28 36"; col["ballL"] = "255 170 170"
    col["stick"] = "40 40 52"; col["stickL"] = "90 90 110"
    col["base"] = "58 64 98"; col["baseD"] = "36 40 66"; col["baseL"] = "88 96 140"
    col["btnA"] = "60 220 255"; col["btnB"] = "255 70 160"; col["btnAL"] = "190 248 255"; col["btnBL"] = "255 190 225"

    for (y = 0; y < N; y++) for (x = 0; x < N; x++) px[x, y] = "bg"

    # CRT: casing, bezel, screen with scanlines, stand
    rect(3, 4, 23, 21, "case"); rect(4, 5, 22, 5, "caseL"); rect(23, 5, 23, 21, "caseD"); rect(3, 21, 23, 21, "caseD")
    rect(5, 6, 21, 18, "scr")
    for (y = 7; y <= 18; y += 2) rect(5, y, 21, y, "scan")
    rect(6, 7, 7, 8, "glow")
    # SCREEN=panes: a four-pane window; tri: a gradient triangle; triwin: a triangle outline
    # with a small window inside
    if (SCREEN == "tri" || SCREEN == "triwin") {
        # a near-equilateral triangle (steps of 1, 1, 2 rows), apex (11,7), base y 16: left of the
        # stick's ball, which covers the screen's right side
        cx = 12
        for (y = 7; y <= 16; y++) {
            hw = int((y - 7 + 1) * 2 / 3)
            for (x = cx - hw; x <= cx + hw; x++) {
                edge = (x == cx - hw || x == cx + hw || y == 16)
                if (SCREEN == "tri") px[x, y] = (x < cx ? "p1" : x > cx ? "p4" : "triL")
                else if (edge) px[x, y] = "p2"
            }
        }
        if (SCREEN == "tri") { px[cx, 7] = "p3"; px[cx, 8] = "p3" }
        if (SCREEN == "triwin") {
            rect(cx - 2, 12, cx - 1, 12, "p1"); rect(cx + 1, 12, cx + 2, 12, "p3")
            rect(cx - 2, 14, cx - 1, 14, "p4"); rect(cx + 1, 14, cx + 2, 14, "p1")
            rect(cx - 2, 13, cx - 1, 13, "p1"); rect(cx + 1, 13, cx + 2, 13, "p3")
            rect(cx - 2, 15, cx - 1, 15, "p4"); rect(cx + 1, 15, cx + 2, 15, "p1")
        }
    } else {
        rect(8, 8, 12, 11, "p1"); rect(14, 8, 18, 11, "p2"); rect(8, 13, 12, 16, "p3"); rect(14, 13, 18, 16, "p4")
    }
    # knobs on the casing
    rect(19, 19, 20, 19, "caseD"); rect(17, 19, 17, 19, "caseD")
    # stand
    rect(9, 22, 17, 22, "caseD"); rect(7, 23, 19, 24, "case"); rect(7, 24, 19, 24, "caseD")

    # arcade stick in front, lower right: base, buttons, shaft, ball
    rect(17, 25, 30, 29, "base"); rect(17, 25, 30, 25, "baseL"); rect(17, 29, 30, 29, "baseD")
    button(18, 26, "btnA", "btnAL"); button(26, 26, "btnB", "btnBL")
    rect(23, 17, 24, 25, "stick"); rect(23, 17, 23, 24, "stickL")
    disc(23.5, 14, 4.2, "ball"); disc(24.6, 15.2, 3, "ballD"); disc(23.5, 14, 3.1, "ball"); disc(22.2, 12.6, 1.1, "ballL")

    # 1-pixel dark outline around every object
    for (y = 0; y < N; y++) for (x = 0; x < N; x++) {
        c = px[x, y]
        if (c != "bg" && c != "bg2") continue
        for (d = 0; d < 4; d++) {
            nx = x + (d == 0) - (d == 1); ny = y + (d == 2) - (d == 3)
            if (nx < 0 || ny < 0 || nx >= N || ny >= N) continue
            n = px[nx, ny]
            if (n != "bg" && n != "bg2" && n != "K") { out[x, y] = 1; break }
        }
    }
    for (k in out) px[k] = "K"

    # PAD=n: a border of background (the Android adaptive icon's foreground: PAD=11, 54x54).
    # MODE=art: the 32x32 art (PPM). MODE=app: the same with pixel-rounded transparent corners (PAM,
    # RGBA). MODE=discord: centred in a 40x40 background (Discord crops server icons to a circle).
    if (MODE == "app") {
        print "P7"; print "WIDTH " N; print "HEIGHT " N; print "DEPTH 4"; print "MAXVAL 255"; print "TUPLTYPE RGB_ALPHA"; print "ENDHDR"
        for (y = 0; y < N; y++) for (x = 0; x < N; x++) {
            dx = (x < 3 ? 3 - x : (x > N - 4 ? x - (N - 4) : 0)); dy = (y < 3 ? 3 - y : (y > N - 4 ? y - (N - 4) : 0))
            clear = dx * dx + dy * dy > 9 # a radius-3 corner, in pixels
            split(col[px[x, y]], rgb, " ")
            printf "%c%c%c%c", rgb[1] + 0, rgb[2] + 0, rgb[3] + 0, clear ? 0 : 255
        }
        exit
    }
    pad = (MODE == "discord" ? 4 : PAD + 0); M = N + 2 * pad
    print "P3"; print M, M; print 255
    for (y = 0; y < M; y++) {
        line = ""
        for (x = 0; x < M; x++) {
            ax = x - pad; ay = y - pad
            line = line (ax < 0 || ay < 0 || ax >= N || ay >= N ? col["bg"] : col[px[ax, ay]]) " "
        }
        print line
    }
}
