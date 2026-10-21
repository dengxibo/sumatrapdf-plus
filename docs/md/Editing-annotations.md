# Editing annotations

**Available in version 3.3 or later.**

You can add / edit annotations in PDF files.

## Shape opacity and line width

In the annotation editor, rectangles and circles support **Opacity** from 0% (invisible) to 100% (opaque).
Opacity affects both the border and fill. Choose a fill color and lower the opacity to mark an area on a scanned page without relying on its OCR text layer.

Rectangles, circles, lines, and ink support border widths from 0 to 12 PDF points in 0.25-point steps, including 0.25, 0.5, and 0.75 pt. Zero retains the hairline behavior. Existing widths are displayed without rewriting the annotation when it is selected.

The last opacity, fill, color, and line width are remembered separately for each annotation type. Restoring defaults returns shape opacity to 100% and restores the usual default border and fill.

New text highlights use the same page geometry as the visible text selection, including its configured height. Saved highlights keep these coordinates when reopened; existing annotations are not resized automatically. Incorrect OCR text positions in the source PDF can still require recognizing the page again.

## Highlight text with `a`

Most common annotation is highlighting of text: select text and press `a`. This creates a highlight annotation in yellow color:

![Unsaved Annotation Changes](img/annot-unsaved-changes.png)

Here I highlighted word `USAGE` and pressed `a`.

## Saving annotations back to PDF file

Notice in toolbar (upper right) we show: You have unsaved annotations.

When you close the document (or exit the app) and have unsaved annotations, SumatraPDF will ask if you want to save them:

![Unsaved Annotations Dialog](img/unsaved-annotations-dialog.png)

If you close the dialog or choose `Discard`, annotations will be lost.

`Save changes to existing PDF` will over-write the PDF with newly added annotations.

`Save changes to a new PDF` will allow you to save as a new file.

## Open annotation editor

If you press `A` (upper case i.e. `Shift-a`) we will create a highlight annotation (same as lower case `a`) but also open annotation editor window.

Another way to open annotation editor is to use context menu (right mouse click or `Shift-F10`) and use `Edit Annotations`:

![Annotation Context Menu](img/annot-context-menu.png)

## Other annotations for selected text

When you selected a text, you can create the following annotations from the selection:

- highlight
- underline
- strike out
- squiggly underline

![Context Menu Annotation From Selection](img/context-menu-annot-from-sel.png)

We also copy selected text to a clipboard so that e.g. you can use `Ctrl-V` to paste it into `Contents` property of the annotation.

## Other annotation types

You can also create annotation object at your mouse location:

- text
- free text
- stamp
- caret

![Context Menu Annotation Under Cursor](img/context-menu-annot-under-cursor.png)

## Annotation editor

All those commands will open annotation editor:

![Annotation Editor](img/annotation-editor.png)

Here we have a text annotation in the editor.

You can delete annotations, change color, text and other attributes. Use **Export Notes** to save all annotations to a Markdown file (page, excerpt, note, author, and date).

## Select annotation from page in editor

When you have many annotations on the page, it's hard to locate the desired annotation in the editor list.

To select an annotation in the editor, place the mouse cursor over an annotation in the page, right click for context menu and use `Select Annotation in Editor`.

![Context Menu Select Annotation In Editor](img/context-menu-select-in-editor.png)

## Moving annotations

To move an annotation on the page, left click with mouse and drag to new location.

## Customize highlight color

To change default color for highlight annotation (created with keyboard shortcut `a`), use `Settings` / `Options...` → **Advanced** → `Open Advanced Options File...` and change `Annotations` \ `HighlightColor` value. Default is yellow `#ffff00`).

## EPUB / MOBI annotations

You can also annotate EPUB and MOBI/AZW ebooks. Select text and use the same highlight / underline / squiggly / strike-out commands (or the selection toolbar). Ebook annotations are stored as sidecar JSON files under the SumatraPDF data directory (`Annotations`), not inside the book file, so they survive font-size and layout changes.

Markup style and default colors match PDF annotations (`Annotations` \ `HighlightColor`, `UnderlineColor`, `SquigglyColor`, `StrikeOutColor`). Use `Edit Annotations` to list annotations, edit notes, change colors, or delete them. Selecting the same range and highlighting again toggles the annotation off.

Annotation author is the reader, not the book author. Set the default in **Options → Reading → Annotations**. Leave it empty to use the Windows user name, or type `(none)` to leave the author off new annotations. The annotation editor’s Author field changes only the selected annotation. Use **Export Notes** in the annotation editor to save all annotations to a Markdown file (page, excerpt, note, author, and date).

## Missing features

This is a first version of annotation editing. We don't yet support all annotation types and can't do everything that other PDF editing apps can do.

The future will be driven by your feedback. If there are features missing or there are better ways of doing things, let me know in [Discussions](https://github.com/sumatrapdfreader/sumatrapdf/discussions)

When providing feedback:

- tell us what
- tell us why. Context is important for prioritizing features. Is new feature / idea something that you absolutely need or is it just a nice improvement
- you might be familiar how other PDF editors work. When referencing feature or UI ideas coming from other apps, tell us which app it is. Screenshots are better than words when describing UI ideas.
