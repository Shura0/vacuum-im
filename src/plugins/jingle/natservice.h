#ifndef NATSERVICE_H
#define NATSERVICE_H

#include <QDateTime>
#include <QDomElement>
#include <QList>
#include <QString>

/*
 * External services discovered through XEP-0215 (urn:xmpp:extdisco:2): STUN
 * servers and TURN relays. The plugin asks its own server for the list on
 * stream open, requests short-term credentials for restricted relays, accepts
 * push updates and falls back to the manual settings from the options page.
 */
struct NatService
{
	NatService();

	QString type;        //"turn", "stun"
	QString host;
	quint16 port;
	QString transport;   //"udp" (the only one we use), "tcp"
	QString username;
	QString password;
	QDateTime expires;   //When the credentials stop working, invalid when absent
	bool restricted;     //Credentials have to be requested for this service
	QString action;      //"add", "modify" or "remove" in a push update
};

//The effective configuration handed to the ICE agent: the manual settings from
//the options page take precedence, otherwise the first usable discovered
//service is used.
struct NatConfig
{
	NatConfig();

	QString stunHost;
	quint16 stunPort;
	QString turnHost;
	quint16 turnPort;
	QString turnUsername;
	QString turnPassword;
	bool forceRelay;

	bool hasStun() const;
	bool hasTurn() const;
};

namespace NatDiscovery
{
//Parses <services><service .../></services> (a full response or a push)
QList<NatService> parseServices(const QDomElement &AServicesElem);
//Parses <credentials><service .../></credentials>
QList<NatService> parseCredentials(const QDomElement &ACredentialsElem);
//Applies a push update to a cached list: remove/modify/add by type+host+port
void applyUpdate(QList<NatService> &AServices, const QList<NatService> &AUpdate);

QString serviceKey(const NatService &AService);
QString describe(const NatService &AService);

//A relay we can actually allocate on: UDP transport with a host and a port
bool isUsableTurn(const NatService &AService);
bool isUsableStun(const NatService &AService);
}

#endif // NATSERVICE_H
