#ifndef MESSAGEMARKS_H
#define MESSAGEMARKS_H

#include <QChar>

// Invisible bounds of the copyable message body. The style engine places
// them around the body only, so the sender, the time and the marks stay
// outside and are dropped when the selection is copied as plain text.
// The mark plugins use U+200B..U+200D and U+2060..U+206F as their invisible
// anchors, so the bounds must be different characters.
#define SMS_MESSAGE_BODY_START             QChar(0xFFF9)
#define SMS_MESSAGE_BODY_END               QChar(0xFFFA)

// Reserved content slot filled with the message marks (delivery receipts,
// chat markers, the edited note and the OMEMO status).
#define MSM_CONTENT_SLOT_MARKS              "marks"

// CSS classes used to paint the message marks. A style variant defines their
// appearance in its Variants/*.css file, the plugins only emit the class names.
#define MSMC_MARK_PENDING                   "vacuum-marks-pending"
#define MSMC_MARK_DELIVERED                 "vacuum-marks-delivered"
#define MSMC_MARK_DISPLAYED                 "vacuum-marks-displayed"
#define MSMC_MARK_EDITED                    "vacuum-marks-edited"
#define MSMC_MARK_OMEMO                     "vacuum-marks-omemo"
#define MSMC_MARK_OMEMO_OPEN                "vacuum-marks-omemo-open"
#define MSMC_MARK_OMEMO_UNENCRYPTED         "vacuum-marks-omemo-unencrypted"
#define MSMC_MARK_OMEMO_FAILED              "vacuum-marks-omemo-failed"
#define MSMC_MARK_OMEMO_UNTRUSTED           "vacuum-marks-omemo-untrusted"

#endif // MESSAGEMARKS_H
