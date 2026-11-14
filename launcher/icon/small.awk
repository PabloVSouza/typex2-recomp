# The small icon (Discord server, favicon sizes): one bold shape on a 24x24 grid, as icon.awk's
# 32x32 scene is too busy at 48 px. SHAPE=emblem: the triangle with the window; crt: a CRT front
# with the emblem on its screen; stick: the emblem with the ball-top stick beside it.
# awk -v SHAPE=emblem -f small.awk > small.ppm   (P3, 24x24; PAD=n adds a background border)
function rect(x0, y0, x1, y1, c,   x, y) { for (y = y0; y <= y1; y++) for (x = x0; x <= x1; x++) px[x, y] = c }
function disc(cx, cy, r, c,   x, y) {
    for (y = int(cy - r - 1); y <= cy + r + 1; y++)
        for (x = int(cx - r - 1); x <= cx + r + 1; x++)
            if ((x - cx) ^ 2 + (y - cy) ^ 2 <= r * r) px[x, y] = c
}
# a triangle (apex x cx, rows y0..y1, a pixel wider per side every 1-1-2 rows... here: per
# `step` rows) drawn as a thick outline, with the four-pane window inside
function emblem(cx, y0, y1, thick, pane,   y, x, hw, inner) {
    for (y = y0; y <= y1; y++) {
        hw = int((y - y0 + 1) * 0.62)
        for (x = cx - hw; x <= cx + hw; x++) {
            inner = (x > cx - hw + thick - 1 && x < cx + hw - thick + 1 && y < y1 - thick + 1)
            if (!inner) px[x, y] = "tri"
        }
    }
    # panes: pane x pane each, one pixel apart, centred, a pixel above the base band
    py = y1 - thick - 2 * pane - 1
    rect(cx - pane, py, cx - 1, py + pane - 1, "p1"); rect(cx + 1, py, cx + pane, py + pane - 1, "p3")
    rect(cx - pane, py + pane + 1, cx - 1, py + 2 * pane, "p4"); rect(cx + 1, py + pane + 1, cx + pane, py + 2 * pane, "p1")
}
BEGIN {
    N = 24
    col["bg"] = "27 30 62"; col["K"] = "12 12 22"
    col["tri"] = "60 220 255"; col["p1"] = "255 70 160"; col["p3"] = "255 214 70"; col["p4"] = "150 100 255"
    col["case"] = "214 207 190"; col["caseD"] = "150 144 130"; col["caseL"] = "240 236 224"; col["scr"] = "14 30 40"
    col["ball"] = "232 52 60"; col["ballD"] = "160 28 36"; col["ballL"] = "255 170 170"; col["stick"] = "40 40 52"
    for (y = 0; y < N; y++) for (x = 0; x < N; x++) px[x, y] = "bg"

    if (SHAPE == "crt") {
        rect(2, 3, 21, 19, "case"); rect(3, 4, 20, 4, "caseL"); rect(21, 4, 21, 19, "caseD"); rect(2, 19, 21, 19, "caseD")
        rect(4, 5, 19, 17, "scr")
        emblem(12, 6, 16, 1, 2)
        rect(8, 20, 15, 21, "caseD")
    } else if (SHAPE == "stick") {
        emblem(9, 4, 20, 2, 2)
        rect(18, 11, 19, 20, "stick")
        disc(18.5, 8, 3.6, "ball"); disc(19.4, 9, 2.6, "ballD"); disc(18.5, 8, 2.7, "ball"); disc(17.4, 6.8, 0.9, "ballL")
    } else {
        emblem(12, 3, 20, 2, 3)
    }

    # 1-pixel dark outline around shapes on the background
    for (y = 0; y < N; y++) for (x = 0; x < N; x++) {
        if (px[x, y] != "bg") continue
        for (d = 0; d < 4; d++) {
            nx = x + (d == 0) - (d == 1); ny = y + (d == 2) - (d == 3)
            if (nx < 0 || ny < 0 || nx >= N || ny >= N) continue
            if (px[nx, ny] != "bg" && px[nx, ny] != "K") { out[x, y] = 1; break }
        }
    }
    for (k in out) px[k] = "K"

    pad = PAD + 0; M = N + 2 * pad
    print "P3"; print M, M; print 255
    for (y = 0; y < M; y++) {
        line = ""
        for (x = 0; x < M; x++) {
            ax = x - pad; ay = y - pad
            line = line (ax < 0 || ay < 0 || ax >= N || ay >= N ? col["bg"] : col[px[int(ax), int(ay)]]) " "
        }
        print line
    }
}
