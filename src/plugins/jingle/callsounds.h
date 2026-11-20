#ifndef CALLSOUNDS_H
#define CALLSOUNDS_H

#include <QObject>

class QSoundEffect;
class INotifications;

/*
 * Short sounds of a call session: the ringtone that loops while an incoming
 * call waits for an answer, the dial tone that loops while an outgoing call
 * waits for the peer, and the "call ended" tone played when a call that was
 * actually established is over. The WAV files are loaded from the shared
 * "sounds" storage, the same way the notifications plugin loads its sounds.
 * The ringtone obeys the incoming-call event on the Notifications page, while
 * the dial tone and the end-of-call tone always play; without the storage all
 * stay silent.
 */
class CallSounds :
	public QObject
{
	Q_OBJECT;
public:
	explicit CallSounds(INotifications *ANotifications, QObject *AParent = nullptr);

	void playRingtone();
	void stopRingtone();
	void playDialtone();
	void stopDialtone();
	void playHangup();

private:
	bool isSoundEnabled(const QString &ATypeId);
	bool loadEffect(QSoundEffect *AEffect, const QString &AKey);
	static QString soundFile(const QString &AKey);

private:
	INotifications *FNotifications;
	QSoundEffect *FRingtone;
	QSoundEffect *FDialtone;
	QSoundEffect *FHangup;
	bool FStorageWarned;
};

#endif // CALLSOUNDS_H
