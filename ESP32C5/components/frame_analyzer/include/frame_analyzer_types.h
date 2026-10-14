#ifndef FRAME_ANALYZER_TYPES_H
#define FRAME_ANALYZER_TYPES_H

#include <stdint.h>

/* IEEE 802.1X/Ethernet EAPOL EtherType. Parsed with bounded byte reads. */
#define ETHER_TYPE_EAPOL 0x888eU

typedef enum {
    EAPOL_EAP_PACKET = 0,
    EAPOL_START,
    EAPOL_LOGOFF,
    EAPOL_KEY,
    EAPOL_ENCAPSULATED_ASF_ALERT,
    EAPOL_MKA,
    EAPOL_ANNOUNCEMENT_GENERIC,
    EAPOL_ANNOUNCEMENT_SPECIFIC,
    EAPOL_ANNOUNCEMENT_REQ
} eapol_packet_types_t;

#endif
