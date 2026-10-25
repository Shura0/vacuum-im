#ifndef MESSAGEMARKS_H
#define MESSAGEMARKS_H

// Reserved content slot filled with the message marks (delivery receipts,
// chat markers and the edited note).
#define MSM_CONTENT_SLOT_MARKS              "marks"

// CSS classes used to paint the message marks. A style variant defines their
// appearance in its Variants/*.css file, the plugins only emit the class names.
#define MSMC_MARK_PENDING                   "vacuum-marks-pending"
#define MSMC_MARK_DELIVERED                 "vacuum-marks-delivered"
#define MSMC_MARK_DISPLAYED                 "vacuum-marks-displayed"
#define MSMC_MARK_EDITED                    "vacuum-marks-edited"

#endif // MESSAGEMARKS_H
