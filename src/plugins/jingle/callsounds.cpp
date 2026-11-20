#include "callsounds.h"

#include <QSoundEffect>
#include <QUrl>

#include <definitions/notificationtypes.h>
#include <definitions/resources.h>
#include <interfaces/inotifications.h>
#include <utils/filestorage.h>
#include <utils/logger.h>

CallSounds::CallSounds(INotifications *ANotifications, QObject *AParent) : QObject(AParent)
{
	FNotifications = ANotifications;
	FStorageWarned = false;

	//The WAV itself contains the pause between the rings, so a plain endless
	//loop gives "ring - pause - ring - pause"
	FRingtone = new QSoundEffect(this);
	FRingtone->setLoopCount(QSoundEffect::Infinite);
	FRingtone->setVolume(0.9);

	//A single "dialing" blip followed by the silence the file already contains
	FDialtone = new QSoundEffect(this);
	FDialtone->setLoopCount(QSoundEffect::Infinite);
	FDialtone->setVolume(0.9);

	FHangup = new QSoundEffect(this);
	FHangup->setLoopCount(1);
	FHangup->setVolume(0.9);
}

bool CallSounds::isSoundEnabled(const QString &ATypeId)
{
	//Both the ringtone and the dial tone follow their event on the Notifications
	//page, the same checkbox that controls the sound of any notification type
	if (FNotifications == nullptr)
		return true;
	return (FNotifications->enabledTypeNotificationKinds(ATypeId) & INotification::SoundPlay) > 0;
}

QString CallSounds::soundFile(const QString &AKey)
{
	FileStorage *storage = FileStorage::staticStorage(RSR_STORAGE_SOUNDS);
	return storage!=nullptr ? storage->fileFullName(AKey) : QString();
}

bool CallSounds::loadEffect(QSoundEffect *AEffect, const QString &AKey)
{
	if (AEffect->source().isEmpty())
	{
		const QString file = soundFile(AKey);
		if (file.isEmpty())
		{
			if (!FStorageWarned)
			{
				FStorageWarned = true;
				LOG_WARNING(QString("Call sounds: the '%1' sound is not available in the sounds storage, "
				                    "call sounds are disabled").arg(AKey));
			}
			return false;
		}
		AEffect->setSource(QUrl::fromLocalFile(file));
	}
	return AEffect->status()!=QSoundEffect::Error;
}

void CallSounds::playRingtone()
{
	const bool enabled = isSoundEnabled(NNT_JINGLE_CALL);
	LOG_DEBUG(QString("CallSounds: play ringtone, enabled=%1, file='%2'").
		arg(enabled).arg(soundFile(QStringLiteral("jingleRingtone"))));
	if (enabled && loadEffect(FRingtone, QStringLiteral("jingleRingtone")))
		FRingtone->play();
}

void CallSounds::stopRingtone()
{
	FRingtone->stop();
}

void CallSounds::playDialtone()
{
	stopRingtone();
	if (FDialtone->isPlaying())
		return;
	if (loadEffect(FDialtone, QStringLiteral("jingleDialtone")))
		FDialtone->play();
}

void CallSounds::stopDialtone()
{
	FDialtone->stop();
}

void CallSounds::playHangup()
{
	//The end-of-call tone is not a preference: it always plays, so that a call
	//never ends silently when the sounds are turned off
	stopRingtone();
	stopDialtone();
	if (loadEffect(FHangup, QStringLiteral("jingleHangup")))
		FHangup->play();
}
