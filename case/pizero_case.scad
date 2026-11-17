// ============================================================================
// Raspberry Pi Zero W case for kl-zero-stick, v7 (low, USB-A next to the micro-USB)
// The USB-A plug stands perpendicular to the large face, NEXT TO the board (in the
// port strip, beside the micro-USB), so its back rests directly on the base plate.
// Only the metal shell passes through the face; the plastic body stays inside.
// The case plugs straight into the roaster's USB-A port.
//
// Parts:
//  - SHELL: face + walls, narrow slot for the metal, reinforcing frame around the
//    plug body, 2 posts with M5 nut traps, Pi posts, small teeth that keep the
//    micro-USB plug seated.
//  - PLATE: bottom, Pi standoffs, local pad under the USB-A with a channel for a
//    flat cable. 2 M5 bolts from below into nuts in the shell clamp plate + USB-A
//    + face together. 4 x M2.5x12 screws hold the Pi.
//
// Cable: a long, thin, flexible flat USB-A to micro-USB cable. MEASURE your plugs
// and set the values below.
//
// Board (top view): ports towards +Y, GPIO header on the -Y side, microSD at +X.
// ============================================================================

/* [View] */
part = "assembly"; // [assembly, section, print, shell, plate]

/* [USB-A (MEASURE)] */
usba_body_l = 18.0;     // MEASURED: from where the cable stiffens to the metal
usba_body_w = 15.6;     // (including +0.3 tolerance), along Y
usba_body_t = 7.6;      // along X
usba_metal_w = 12.0;
usba_metal_t = 4.5;
usba_metal_l = 12.0;
metal_tol = 0.3;
chan_w = 7.0;           // cable channel in the plate
chan_d = 1.2;

/* [micro-USB (MEASURE)] */
micro_plug_out = 20.5;
micro_body_w = 11.0;
micro_body_t = 7.0;
cable_fold = 2.5;
cable_w = 6.0;
keeper_gap = 0.3;

/* [Raspberry Pi Zero: official dimensions, port positions checked against a printed case] */
rpi_l = 65.0;
rpi_w = 30.0;
rpi_pcb_h = 1.5;
rpi_clearance = 0.6;
standoff_h = 3.0;
hole_inset = 3.5;
usb_data_from_sd = 41.4;
micro_usb_zc = 1.35;
socket_h = 2.7;
hdmi_h = 3.2;
header_h = 8.5;
sd_cut_w = 18.0;
micro_gap = 0.6;        // gap micro-USB -> USB-A plastic body
edge_gap = 1.5;         // USB-A distance from the board edge (the HDMI socket sticks out a little)

/* [M5 bolts] */
bolt_l = 16;            // M5 length under the head; accepted range: see ECHO
bolt_d = 5.5;
nut_af = 8.4;
nut_h = 4.3;
post_d = 12.0;
clamp_gap = 1.0;

/* [Shell, plate] */
wall = 2.0;
top_t = 2.0;
floor_t = 1.6;
pad_t = 2.5;            // plate thickness under the USB-A
plate_tol = 0.25;
corner_r = 3.0;
pipost_d = 5.0;
standoff_d = 5.5;
screw_clear_d = 2.8;
screw_tap_d = 2.2;
screw_head_d = 5.0;
frame_w = 2.5;
frame_d = 6.0;
$fn = 40;

// ============================================================================
// Derived values
// ============================================================================
rpi_x0 = wall + rpi_clearance;
rpi_y0 = wall + rpi_clearance;
pcb_z0 = floor_t + standoff_h;
pcb_z1 = pcb_z0 + rpi_pcb_h;
y_b = rpi_y0 + rpi_w;
x_m = rpi_x0 + rpi_l - usb_data_from_sd;
z_m = pcb_z1 + micro_usb_zc;

out_l = rpi_l + 2 * rpi_clearance + 2 * wall;
out_w = y_b + micro_plug_out + cable_fold + wall;
in_x1 = out_l - wall;

x_u = x_m + micro_body_w / 2 + micro_gap + usba_body_t / 2;   // right next to the micro-USB
y_u = y_b + edge_gap + usba_body_w / 2;
// left bolt ON THE OTHER SIDE of the micro-USB, right bolt next to the USB-A
bolts = [[x_m - micro_body_w / 2 - 0.7 - post_d / 2, y_u],
         [x_u + usba_body_t / 2 + 1.0 + post_d / 2, y_u]];
bx0 = bolts[0][0]; bx1 = bolts[1][0];

z_rear = pad_t;
z_face = z_rear + usba_body_l;
box_h = z_face + top_t;
z_tip = bolt_l;                                        // head below the plate (z=0)
z_nut0 = z_tip - nut_h - 0.2;

echo(str("OUTSIDE: ", out_l, " x ", out_w, " x ", box_h, " mm; + metal outside ", usba_metal_l, " mm"));
echo(str("USB-A at (", x_u, ", ", y_u, "); face centre (", out_l / 2, ", ", out_w / 2, ")"));
echo(str("Accepted M5 lengths: ", ceil(pad_t + clamp_gap + nut_h + 0.2), "-", floor(z_face - 0.5), " mm; head OUTSIDE the plate"));

assert(bx0 - post_d / 2 > wall + 0.3, "The left M5 post does not fit");
assert(bx1 + post_d / 2 < in_x1 - 0.3, "The right M5 post does not fit");
assert(z_tip <= z_face - 0.5, "M5 bolt too long: it would come through the face");
assert(z_nut0 - clamp_gap >= pad_t, "M5 bolt too short");
assert(z_face > pcb_z1 + header_h + 0.5, "The GPIO header does not fit");
assert(y_u + usba_body_w / 2 < out_w - wall - 0.3, "The USB-A does not fit in the port strip");

// ============================================================================
module rounded_rect(l, w, h, r) {
    hull() for (x = [r, l - r], y = [r, w - r])
        translate([x, y, 0]) cylinder(r = r, h = h);
}

function holes() = [
    for (dx = [hole_inset, rpi_l - hole_inset], dy = [hole_inset, rpi_w - hole_inset])
        [rpi_x0 + dx, rpi_y0 + dy]
];

module hex(af, h) cylinder(d = af / cos(30), h = h, $fn = 6);

// ============================================================================
// Shell
// ============================================================================
module shell() {
    difference() {
        union() {
            difference() {
                rounded_rect(out_l, out_w, box_h, corner_r);
                translate([wall, wall, -1])
                    rounded_rect(out_l - 2 * wall, out_w - 2 * wall, box_h - top_t + 1, 1.0);
            }
            for (p = holes()) translate([p[0], p[1], pcb_z1]) cylinder(d = pipost_d, h = box_h - pcb_z1 - 0.1);
            // M5 nut posts
            for (b = bolts) translate([b[0], b[1], z_nut0]) cylinder(d = post_d, h = z_face - z_nut0 + 0.1);
            // reinforcing frame around the plug body + bridge ABOVE the micro-USB
            // to the left post (all tied together under the face)
            translate([bx0, y_u - usba_body_w / 2 - frame_w, z_face - frame_d])
                cube([bx1 - bx0, usba_body_w + 2 * frame_w, frame_d + 0.1]);
            // teeth behind the micro-USB plug
            for (s = [-1, 1]) {
                xi = x_m + s * (cable_w / 2 + 0.5);
                xo = x_m + s * (micro_body_w / 2 + 1.0);
                translate([min(xi, xo), y_b + micro_plug_out + keeper_gap, z_m - micro_body_t / 2])
                    cube([abs(xo - xi), out_w - wall + 0.1 - (y_b + micro_plug_out + keeper_gap),
                          box_h - top_t + 0.1 - (z_m - micro_body_t / 2)]);
            }
        }
        for (p = holes()) translate([p[0], p[1], pcb_z1 - 1]) cylinder(d = screw_tap_d, h = 10);
        // pocket for the plug body inside the frame
        translate([x_u - usba_body_t / 2, y_u - usba_body_w / 2, z_face - frame_d - 1])
            cube([usba_body_t, usba_body_w, frame_d + 1]);
        // ONLY the metal passes through
        translate([x_u - (usba_metal_t + metal_tol) / 2, y_u - (usba_metal_w + metal_tol) / 2, z_face - 1])
            cube([usba_metal_t + metal_tol, usba_metal_w + metal_tol, top_t + 2]);
        // M5: nut trap + hole up to just below the face
        for (b = bolts) translate([b[0], b[1], 0]) {
            translate([0, 0, z_nut0 - 0.01]) rotate([0, 0, 30]) hex(nut_af, nut_h + 0.01);
            translate([0, 0, z_nut0 - 1]) cylinder(d = bolt_d, h = z_face - 0.5 - z_nut0 + 1);
        }
        translate([out_l - wall - 1, rpi_y0 + rpi_w / 2 - sd_cut_w / 2, pcb_z0 - 0.5])
            cube([wall + 2, sd_cut_w, rpi_pcb_h + 3.0]);
    }
}

// ============================================================================
// Plate
// ============================================================================
module plate() {
    pl = out_l - 2 * wall - 2 * plate_tol;
    pw = out_w - 2 * wall - 2 * plate_tol;
    difference() {
        union() {
            translate([wall + plate_tol, wall + plate_tol, 0]) rounded_rect(pl, pw, floor_t, 0.8);
            for (p = holes()) translate([p[0], p[1], 0]) cylinder(d = standoff_d, h = pcb_z0);
            // pad under the USB-A and the M5 bolts
            translate([bx0 - post_d / 2, y_u - usba_body_w / 2 - 1.5, 0])
                cube([bx1 - bx0 + post_d, usba_body_w + 3, pad_t]);
            // lip that keeps the back of the USB-A in place
            difference() {
                translate([x_u - usba_body_t / 2 - 1.2, y_u - usba_body_w / 2 - 1.2, 0])
                    cube([usba_body_t + 2.4, usba_body_w + 2.4, pad_t + 1.0]);   // 3.5 mm: passes under the micro-USB (3.95)
                translate([x_u - usba_body_t / 2, y_u - usba_body_w / 2, pad_t])
                    cube([usba_body_t, usba_body_w, 5]);
            }
        }
        // cable channel: from the USB-A centre towards +Y (out into the strip)
        translate([x_u - chan_w / 2, y_u, pad_t - chan_d])
            cube([chan_w, usba_body_w / 2 + 5, chan_d + 3]);
        // M5 clearance
        for (b = bolts) translate([b[0], b[1], -1]) cylinder(d = bolt_d, h = pad_t + 2);
        for (p = holes()) translate([p[0], p[1], 0]) {
            translate([0, 0, -1]) cylinder(d = screw_clear_d, h = pcb_z0 + 2);
            translate([0, 0, -0.01]) cylinder(d1 = screw_head_d, d2 = screw_clear_d, h = (screw_head_d - screw_clear_d) / 2);
        }
        for (i = [0:5])
            translate([rpi_x0 + 14 + i * 6.5, rpi_y0 + 10, -1]) cube([2.2, 14, floor_t + 2]);
    }
}

// ============================================================================
// Ghosts (preview only, not printed)
// ============================================================================
module g_pcb() translate([rpi_x0, rpi_y0, pcb_z0]) cube([rpi_l, rpi_w, rpi_pcb_h]);
module g_header() translate([rpi_x0 + rpi_l / 2 - 25.4, rpi_y0 + hole_inset - 2.54, pcb_z1]) cube([50.8, 5.08, header_h]);
module g_sockets() {
    for (d = [54.0, 41.4]) translate([rpi_x0 + rpi_l - d - 4, y_b - 5, pcb_z1]) cube([8, 5.5, socket_h]);
    translate([rpi_x0 + rpi_l - 12.4 - 5.6, y_b - 7.5, pcb_z1]) cube([11.2, 8.5, hdmi_h]);   // +0.5 overhang
}
module g_micro() translate([x_m - micro_body_w / 2, y_b, z_m - micro_body_t / 2]) cube([micro_body_w, micro_plug_out, micro_body_t]);
module g_usba() translate([x_u - usba_body_t / 2, y_u - usba_body_w / 2, z_rear]) cube([usba_body_t, usba_body_w, usba_body_l]);
module g_metal() translate([x_u - usba_metal_t / 2, y_u - usba_metal_w / 2, z_face]) cube([usba_metal_t, usba_metal_w, usba_metal_l]);
module g_bolts() for (b = bolts) translate([b[0], b[1], 0]) {
    translate([0, 0, -4]) cylinder(d = 8.5, h = 4);
    cylinder(d = 5, h = bolt_l);
    translate([0, 0, z_nut0]) rotate([0, 0, 30]) hex(8, 4);
}

module ghosts() {
    color("green", 0.5) g_pcb();
    color("black", 0.6) g_header();
    color("silver") g_sockets();
    color("white", 0.8) g_micro();
    color("white", 0.8) g_usba();
    color("silver") g_metal();
    color("dimgray") g_bolts();
}

module assembly() {
    color("steelblue", 0.35) shell();
    color("orange") plate();
    ghosts();
}

// ============================================================================
module shell_print() translate([0, out_w, box_h]) rotate([180, 0, 0]) shell();
if (part == "shell") shell_print();
else if (part == "plate") plate();
else if (part == "print") {
    shell_print();
    translate([out_l + 8 - wall, -wall, 0]) plate();
}
else if (part == "section")
    intersection() {
        assembly();
        translate([-50, -50, -1]) cube([300, y_u + 50, 100]);
    }
else assembly();
