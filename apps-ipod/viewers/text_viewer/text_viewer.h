/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to text_viewer.c.
 ****************************************************************************/
#ifndef _TEXT_VIEWER_H_
#define _TEXT_VIEWER_H_

/* Core-linked document viewer, built on the ts_* extraction engine (see
 * txt_source.h). Opens `file` -- plain text, markdown, html, rtf, fb2, epub,
 * docx or pdf -- and pages through its text. Returns a GO_TO_* code. */
int text_viewer(const char *file);

#endif /* _TEXT_VIEWER_H_ */
