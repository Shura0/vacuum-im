#ifndef IENDTOENDENCRYPTION_H
#define IENDTOENDENCRYPTION_H

#include <QObject>
#include <utils/jid.h>
#include <utils/message.h>

#define ENDTOENDENCRYPTION_UUID "{B2B11A1E-6C4B-4F6E-9A8E-6C6C1B0F1D10}"

// Small optional hook for message history and other paths that bypass the
// normal stanza pipeline.  The implementation decrypts an E2E encrypted
// message in place; it must be a no-op for messages that are not encrypted.
class IEndToEndEncryption
{
public:
	virtual QObject *instance() =0;
	virtual bool decryptMessage(const Jid &AStreamJid, Message &AMessage, int ADirection) =0;
};

Q_DECLARE_INTERFACE(IEndToEndEncryption,"Vacuum.Plugin.IEndToEndEncryption/1.0");

#endif // IENDTOENDENCRYPTION_H
