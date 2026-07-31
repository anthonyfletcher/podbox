/***************************************************************************
 * GNU General Public License (version 2+)
 *
 * Interface to text_viewer.c.
 ****************************************************************************/
#ifndef _TEXT_VIEWER_H_
#define _TEXT_VIEWER_H_

#include <stdbool.h>
#include <stddef.h>

/* Core-linked document viewer, built on the ts_* extraction engine (see
 * txt_source.h). Opens `file` -- plain text, markdown, html, rtf, fb2, epub,
 * docx or pdf -- and pages through its text. Returns a GO_TO_* code.
 *
 * Where the reader got to is remembered per document and restored on open, so
 * reopening a file resumes it with no help from the caller. */
int text_viewer(const char *file);

/* Path of the most recently read document, for a caller that wants to reopen
 * it. False if nothing has been read, or the record is unusable. The document
 * may since have been deleted; the caller checks. */
bool text_viewer_last_document(char *buf, size_t bufsize);

#endif /* _TEXT_VIEWER_H_ */
