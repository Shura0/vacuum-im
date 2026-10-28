#ifndef IIMAGEMEDIAPROVIDER_H
#define IIMAGEMEDIAPROVIDER_H

#include <QUrl>
#include <functional>

// The bridge between end-to-end encrypted media producers (e.g. the OMEMO
// plugin keeping aesgcm attachment keys) and the image viewer: the viewer
// consults this interface for every image link before downloading anything
// itself, so encrypted media is never fetched in the clear and not
// downloaded twice. The provider resolves the url to a decrypted local
// file and the viewer embeds it just like an ordinary image.
class IImageMediaProvider
{
public:
	virtual QObject *instance() = 0;
	// True when the url is an attachment this provider can decrypt.
	virtual bool canHandleMediaUrl(const QUrl &AUrl) = 0;
	// Resolves the url to the decrypted local file and calls AHandler with
	// its path, or with an empty string when the media cannot be provided.
	// AHandler is called on the main thread, at a later time, exactly once.
	virtual void requestMediaPath(const QUrl &AUrl, const std::function<void(const QString &AFilePath)> &AHandler) = 0;
	// Called by the preview consumer (the image viewer) when it takes care
	// of rendering the provider urls. The provider then must not insert its
	// own explicit image representations for the urls it serves.
	virtual void setMediaConsumerActive(bool AActive) = 0;
};

Q_DECLARE_INTERFACE(IImageMediaProvider, "Vacuum.Plugin.IImageMediaProvider/1.0")

#endif // IIMAGEMEDIAPROVIDER_H
