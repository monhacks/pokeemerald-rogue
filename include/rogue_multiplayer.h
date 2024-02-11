#ifndef ROGUE_MULTIPLAYER_H
#define ROGUE_MULTIPLAYER_H

extern EWRAM_DATA struct RogueNetMultiplayer* gRogueMultiplayer;

void RogueMP_Init();

bool8 RogueMP_IsActive();
bool8 RogueMP_IsActiveOrConnecting();
bool8 RogueMP_IsConnecting();
bool8 RogueMP_IsHost();
bool8 RogueMP_IsClient();
u8 RogueMP_GetLocalPlayerId();
u8 RogueMP_GetRemotePlayerId();

u8 RogueMP_GetPlayerOutfitId(u8 playerId);
u16 RogueMP_GetPlayerOutfitStyle(u8 playerId, u8 outfitStyle);

void RogueMP_OpenHost();
void RogueMP_OpenClient();
void RogueMP_Close();

void RogueMP_MainCB();
void RogueMP_OverworldCB();

u8 RogueMP_WaitForConnection();

#endif