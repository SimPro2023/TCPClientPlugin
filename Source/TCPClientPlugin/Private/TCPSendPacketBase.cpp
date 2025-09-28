#include "TCPSendPacketBase.h"
#include "Templates/SubclassOf.h"
#include "UObject/UObjectGlobals.h" // Needed for NewObject<>

void UTCPSendPacketBase::ConvertToBytes(TCPBufferWriter& writer)
{
    BufferWriter = &writer;
    ConvertToBytesBP();
}

UTCPSendPacketBase* UTCPSendPacketBase::CreateSendPacketBP(TSubclassOf<UTCPSendPacketBase> packet)
{
    if (!packet) return nullptr; // Pruefen, ob Subclass gueltig

    // packet.Get() liefert den UClass*, korrekt fuer NewObject
    UTCPSendPacketBase* newObject = NewObject<UTCPSendPacketBase>(GetTransientPackage(), packet.Get());
    return newObject;
}
