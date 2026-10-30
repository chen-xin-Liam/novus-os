//! The left-hand device tree: flattening the parent-linked node list into the
//! visible rows, painting it, and hit-testing it.
//!
//! The kernel sends a flat list with `parent` links, not a tree. Turning that
//! into the rows the user sees is a single recursive walk that skips
//! collapsed nodes; the result is kept in a fixed array so the app has no
//! allocator. Painting and hit-testing live here too, both geometrically
//! simple because every row is `ROW_H` tall.

use crate::sinfo::DevRec;
use crate::ui;
use crate::wm;
use crate::devmgr::fields::*;

pub const MAX_NODES: usize = 48;
pub const ROW_H: i16 = 20;

pub struct Row {
    pub idx: usize, /* index into the caller's node array */
    pub depth: u8,
    pub cat: u8, /* category, for the swatch colour */
    pub has_child: bool,
}

/// Walk `nodes` and append every visible row (respecting `expanded`) to
/// `rows`, returning the count. `parent` is the node index (or 0xFFFF for the
/// root) whose children we are listing.
fn walk(
    nodes: &[DevRec; MAX_NODES],
    n: usize,
    has_child: &[bool; MAX_NODES],
    expanded: u64,
    parent: u16,
    depth: u8,
    rows: &mut [Row; MAX_NODES],
    count: &mut usize,
) {
    for i in 0..n {
        if nodes[i].parent != parent {
            continue;
        }
        if *count >= rows.len() {
            return;
        }
        let cat = if nodes[i].kind == KIND_CATEGORY {
            nodes[i].class_id
        } else {
            let p = nodes[i].parent as usize;
            if p < n {
                nodes[p].class_id
            } else {
                CAT_SYS
            }
        };
        rows[*count] = Row {
            idx: i,
            depth,
            cat,
            has_child: has_child[i],
        };
        *count += 1;
        if expanded & (1u64 << i) != 0 && has_child[i] {
            walk(nodes, n, has_child, expanded, i as u16, depth + 1, rows, count);
        }
    }
}

/// Build the visible row list. `root_parent` is the parent value the root row
/// carries (0xFFFF for DEVTAB_NONE).
pub fn flatten(
    nodes: &[DevRec; MAX_NODES],
    n: usize,
    expanded: u64,
    root_parent: u16,
    rows: &mut [Row; MAX_NODES],
) -> usize {
    let mut has_child = [false; MAX_NODES];
    let mut count = 0usize;

    for i in 0..n {
        for j in 0..n {
            if nodes[j].parent == i as u16 {
                has_child[i] = true;
                break;
            }
        }
    }
    walk(nodes, n, &has_child, expanded, root_parent, 0, rows, &mut count);
    count
}

/// Sentinel "no row selected / hovered".
pub const NO_ROW: usize = 0xFFFF_FFFF;

/// Paint the tree inside the rectangle `(x, y, w, h)`. `scroll` is the index
/// of the first visible row; `sel` and `hover` are row indices or NO_ROW.
pub fn draw(
    w: &mut wm::Win,
    x: i16,
    y: i16,
    ww: i16,
    h: i16,
    nodes: &[DevRec; MAX_NODES],
    n: usize,
    rows: &[Row],
    count: usize,
    expanded: u64,
    scroll: usize,
    sel: usize,
    hover: usize,
) {
    w.fill(x, y, ww, h, ui::bg());
    let visible = ((h - 4) / ROW_H) as usize;
    let end = (scroll + visible).min(count);

    for r in scroll..end {
        let row = &rows[r];
        let ry = y + 2 + ((r - scroll) as i16) * ROW_H;
        let indent = 8 + (row.depth as i16) * 14;
        let node = &nodes[row.idx];
        let open = expanded & (1u64 << row.idx) != 0;

        if r == sel {
            w.round_fill(x + 2, ry, ww - 4, ROW_H - 2, 4, ui::row_sel());
            w.round_fill(x + 2, ry + 4, 3, ROW_H - 10, 1, ui::accent());
        } else if r == hover {
            w.round_fill(x + 2, ry, ww - 4, ROW_H - 2, 4, ui::subtle());
        }

        // expand arrow for any node that has children
        if row.has_child {
            w.text(x + indent - 8, ry + 5, ui::dim(), if open { "v" } else { ">" });
        }
        // category / device swatch
        w.round_fill(x + indent, ry + 4, 11, 11, 3, cat_color(row.cat));

        let label = node.name();
        let warn = node.status == ST_WARN;
        if warn {
            w.text(x + indent + 16, ry + 5, ui::warn(), label);
        } else {
            w.text(x + indent + 16, ry + 5, ui::text(), label);
        }

        // problem badge on the warning rows, matching the yellow "!" in the
        // screenshot
        if warn && row.depth <= 1 {
            w.text(x + ww - 22, ry + 5, ui::warn(), "!");
        }
        let _ = (n, open);
    }
}

/// Which row is under `(px, py)`, or NO_ROW. Also reports whether the click
/// landed on the expand arrow (the left 16 px) so the caller can toggle.
pub fn hit(
    x: i16,
    y: i16,
    h: i16,
    count: usize,
    scroll: usize,
    px: i32,
    py: i32,
) -> (usize, bool) {
    let visible = ((h - 4) / ROW_H) as usize;
    let end = (scroll + visible).min(count);
    let top = y + 2;

    if px < x as i32 || py < top as i32 {
        return (NO_ROW, false);
    }
    let row = scroll + ((py - top as i32) / ROW_H as i32) as usize;
    if row >= end {
        return (NO_ROW, false);
    }
    let on_arrow = px < (x + 16) as i32;
    (row, on_arrow)
}

/// Node kind constant (DEV_KIND_CATEGORY) mirrored from kernel/devtab.h.
pub const KIND_CATEGORY: u8 = 2;
/// Root parent sentinel (DEVTAB_NONE).
pub const ROOT_PARENT: u16 = 0xFFFF;
