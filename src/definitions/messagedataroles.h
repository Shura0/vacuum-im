#ifndef DEF_MESSAGEDATAROLERS_H
#define DEF_MESSAGEDATAROLERS_H

enum MessageDataRoles {
	MDR_MESSAGE_ID,
	MDR_MESSAGE_DIRECTION,
	/** Messages marked with this role are archived even when an engine says
	 *  the server archive already keeps them (server MAM fetching). */
	MDR_ARCHIVE_FORCED
};

#endif
