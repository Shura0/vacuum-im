#ifndef MESSAGEMARKS_H
#define MESSAGEMARKS_H

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

#endif // MESSAGEMARKS_H
