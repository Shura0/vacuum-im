#include "natservice.h"

#include <QDomNodeList>

NatService::NatService()
{
	port = 0;
	restricted = false;
}

NatConfig::NatConfig()
{
	stunPort = 0;
	turnPort = 0;
	forceRelay = false;
}

bool NatConfig::hasStun() const
{
	return !stunHost.isEmpty() && stunPort != 0;
}

bool NatConfig::hasTurn() const
{
	return !turnHost.isEmpty() && turnPort != 0;
}

namespace NatDiscovery
{

static NatService parseService(const QDomElement &AServiceElem)
{
	NatService service;
	service.type = AServiceElem.attribute("type").toLower();
	service.host = AServiceElem.attribute("host").trimmed();
	service.port = quint16(AServiceElem.attribute("port").toUShort());
	if (service.port == 0)
	{
		if (service.type == "turn" || service.type == "stun")
			service.port = 3478;
	}
	service.transport = AServiceElem.attribute("transport").toLower();
	service.username = AServiceElem.attribute("username");
	service.password = AServiceElem.attribute("password");
	service.restricted = AServiceElem.attribute("restricted").compare("true", Qt::CaseInsensitive) == 0 ||
		AServiceElem.attribute("restricted").compare("1") == 0;
	service.action = AServiceElem.attribute("action", "add").toLower();

	const QString expires = AServiceElem.attribute("expires");
	if (!expires.isEmpty())
		service.expires = QDateTime::fromString(expires, Qt::ISODate);

	return service;
}

QList<NatService> parseServices(const QDomElement &AServicesElem)
{
	QList<NatService> services;
	if (AServicesElem.isNull())
		return services;

	const QDomNodeList list = AServicesElem.elementsByTagName("service");
	for (int i = 0; i < list.count(); ++i)
	{
		const QDomElement serviceElem = list.at(i).toElement();
		if (serviceElem.isNull() || serviceElem.attribute("host").isEmpty())
			continue;
		const NatService service = parseService(serviceElem);
		if (service.type.isEmpty())
			continue;
		services.append(service);
	}
	return services;
}

QList<NatService> parseCredentials(const QDomElement &ACredentialsElem)
{
	//The payload is <credentials><service host type port username password/></credentials>,
	//the same element shape as a services list
	return parseServices(ACredentialsElem);
}

QString serviceKey(const NatService &AService)
{
	return QString("%1|%2|%3").arg(AService.type, AService.host.toLower()).arg(AService.port);
}

void applyUpdate(QList<NatService> &AServices, const QList<NatService> &AUpdate)
{
	for (const NatService &service : AUpdate)
	{
		const QString key = serviceKey(service);
		int existing = -1;
		for (int i = 0; i < AServices.count(); ++i)
		{
			if (serviceKey(AServices.at(i)) == key)
			{
				existing = i;
				break;
			}
		}

		if (service.action == "remove")
		{
			if (existing >= 0)
				AServices.removeAt(existing);
			continue;
		}

		if (existing >= 0)
		{
			//A modify without new credentials keeps the ones we already have
			NatService merged = service;
			if (merged.username.isEmpty() && merged.password.isEmpty())
			{
				merged.username = AServices.at(existing).username;
				merged.password = AServices.at(existing).password;
				merged.expires = AServices.at(existing).expires;
			}
			AServices[existing] = merged;
		}
		else
		{
			AServices.append(service);
		}
	}
}

QString describe(const NatService &AService)
{
	return QString("%1 %2:%3/%4%5").arg(AService.type, AService.host)
		.arg(AService.port)
		.arg(AService.transport.isEmpty() ? QString("udp") : AService.transport)
		.arg(AService.username.isEmpty() ? QString() : QString(" (credentials)"));
}

bool isUsableTurn(const NatService &AService)
{
	if (AService.type != "turn" || AService.host.isEmpty() || AService.port == 0)
		return false;
	return AService.transport.isEmpty() || AService.transport == "udp";
}

bool isUsableStun(const NatService &AService)
{
	if (AService.type != "stun" || AService.host.isEmpty() || AService.port == 0)
		return false;
	return AService.transport.isEmpty() || AService.transport == "udp";
}

}
