#ifndef OMEMODEFINITIONS_H
#define OMEMODEFINITIONS_H

#define OMEMO_NS                        "eu.siacs.conversations.axolotl"
#define OMEMO_NS_HINTS                  "urn:xmpp:hints"
#define OMEMO_NS_STATUS                 "urn:vacuum:omemo"
#define OMEMO_NODE_DEVICELIST           OMEMO_NS ".devicelist"
#define OMEMO_NODE_BUNDLES              OMEMO_NS ".bundles"
#define OMEMO_NODE_DEVICELIST_NOTIFY    OMEMO_NODE_DEVICELIST "+notify"

#define OMEMO_PREKEYS_MAX               100
#define OMEMO_PREKEYS_MIN               20

#define OMEMO_PEP_REQUEST_TIMEOUT       30000
#define OMEMO_DEVICELIST_TTL            3600

#endif // OMEMODEFINITIONS_H
