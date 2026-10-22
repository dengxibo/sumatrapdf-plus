use std::{ffi::CString, os::raw::c_char, panic::catch_unwind, ptr, slice, str};
use std::sync::OnceLock;

fn normalize_svg(svg: &str) -> Option<String> {
    // MuPDF does not implement SVG markers or per-character x/y/dx/dy lists.
    // Expand both arrows and shaped text into paths to preserve multiline labels.
    static OPTIONS: OnceLock<usvg::Options<'static>> = OnceLock::new();
    let options = OPTIONS.get_or_init(|| {
        let mut options = usvg::Options::default();
        options.fontdb_mut().load_system_fonts();
        options
    });
    let tree = usvg::Tree::from_str(svg, options).ok()?;
    Some(tree.to_string(&usvg::WriteOptions {
        preserve_text: false,
        ..Default::default()
    }))
}

// UTF-8 in, NUL-terminated UTF-8 SVG out. Free through sumatra_mermaid_free.
// Null means invalid input, a renderer error, or a caught Rust panic.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sumatra_mermaid_render(source: *const u8, len: usize) -> *mut c_char {
    if source.is_null() || len == 0 || len > 256 * 1024 {
        return ptr::null_mut();
    }
    catch_unwind(|| {
        let input = str::from_utf8(unsafe { slice::from_raw_parts(source, len) }).ok()?;
        let svg = mermaid_rs_renderer::render(input).ok()?;
        let svg = normalize_svg(&svg)?;
        if svg.len() > 16 * 1024 * 1024 {
            return None;
        }
        Some(CString::new(svg).ok()?.into_raw())
    })
    .ok()
    .flatten()
    .unwrap_or(ptr::null_mut())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn sumatra_mermaid_free(svg: *mut c_char) {
    if !svg.is_null() {
        drop(unsafe { CString::from_raw(svg) });
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CStr;

    #[test]
    fn renders_and_releases_diagrams() {
        for source in [
            "flowchart LR\n A[开始] --> B[结束]",
            "sequenceDiagram\n Alice->>Bob: 你好",
        ] {
            unsafe {
                let svg = sumatra_mermaid_render(source.as_ptr(), source.len());
                assert!(!svg.is_null());
                let text = CStr::from_ptr(svg).to_str().unwrap();
                assert!(text.contains("<svg"));
                assert!(!text.contains("marker-end="));
                assert!(!text.contains("<text"));
                assert!(text.contains("<path"));
                sumatra_mermaid_free(svg);
            }
        }
    }

    #[test]
    fn multiline_labels_keep_separate_baselines() {
        let svg = normalize_svg(r#"<svg xmlns="http://www.w3.org/2000/svg" width="150" height="80">
            <text x="10" y="20" font-family="sans-serif" font-size="14">
            <tspan x="10" dy="0">MMMM</tspan><tspan x="10" dy="24">MMMM</tspan>
            </text></svg>"#).unwrap();
        assert!(!svg.contains("<text"));
        let tree = usvg::Tree::from_str(&svg, &usvg::Options::default()).unwrap();
        assert!(tree.root().abs_bounding_box().height() > 24.0);
    }

    #[test]
    fn rejects_invalid_input() {
        unsafe {
            assert!(sumatra_mermaid_render(ptr::null(), 1).is_null());
            assert!(sumatra_mermaid_render(b"x".as_ptr(), 256 * 1024 + 1).is_null());
            assert!(sumatra_mermaid_render([0xff].as_ptr(), 1).is_null());
            let bad = b"this is not a diagram";
            assert!(sumatra_mermaid_render(bad.as_ptr(), bad.len()).is_null());
            sumatra_mermaid_free(ptr::null_mut());
        }
    }
}
