#include "global.h"
#include "constants/event_objects.h"
#include "constants/event_object_movement.h"
#include "constants/metatile_labels.h"
#include "constants/trainer_types.h"
#include "constants/rogue.h"
#include "gba/isagbprint.h"
#include "event_data.h"
#include "event_object_movement.h"
#include "fieldmap.h"
#include "field_screen_effect.h"
#include "malloc.h"
#include "overworld.h"
#include "random.h"
#include "strings.h"
#include "string_util.h"

#include "rogue.h"
#include "rogue_controller.h"

#include "rogue_adventurepaths.h"
#include "rogue_adventure.h"
#include "rogue_campaign.h"
#include "rogue_settings.h"
#include "rogue_trainers.h"
#include "rogue_query.h"


#define ROOM_TO_WORLD_X 3
#define ROOM_TO_WORLD_Y 2

#define PATH_MAP_OFFSET_X (4)
#define PATH_MAP_OFFSET_Y (4)

#define ADJUST_COORDS_X(val) (gRogueAdvPath.pathLength - val - 1)   // invert so we place the first node at the end
#define ADJUST_COORDS_Y(val) (val - gRogueAdvPath.pathMinY + 1)     // start at coord 0


#define ROOM_TO_METATILE_X(val) ((ADJUST_COORDS_X(val) * ROOM_TO_WORLD_X) + MAP_OFFSET + PATH_MAP_OFFSET_X)
#define ROOM_TO_METATILE_Y(val) ((ADJUST_COORDS_Y(val) * ROOM_TO_WORLD_Y) + MAP_OFFSET + PATH_MAP_OFFSET_Y)

#define ROOM_TO_OBJECT_EVENT_X(val) ((ADJUST_COORDS_X(val) * ROOM_TO_WORLD_X) + PATH_MAP_OFFSET_X + 2)
#define ROOM_TO_OBJECT_EVENT_Y(val) ((ADJUST_COORDS_Y(val) * ROOM_TO_WORLD_Y) + PATH_MAP_OFFSET_Y)

#define ROOM_TO_WARP_X(val) (ROOM_TO_OBJECT_EVENT_X(val) + 1)
#define ROOM_TO_WARP_Y(val) (ROOM_TO_OBJECT_EVENT_Y(val))

#define ROOM_CONNECTION_TOP     0
#define ROOM_CONNECTION_MID     1
#define ROOM_CONNECTION_BOT     2
#define ROOM_CONNECTION_COUNT   3

#define ROOM_CONNECTION_MASK_TOP     (1 << ROOM_CONNECTION_TOP)
#define ROOM_CONNECTION_MASK_MID     (1 << ROOM_CONNECTION_MID)
#define ROOM_CONNECTION_MASK_BOT     (1 << ROOM_CONNECTION_BOT)

#define MAX_CONNECTION_GENERATOR_COLUMNS 5

#define gSpecialVar_ScriptNodeID        gSpecialVar_0x8004
#define gSpecialVar_ScriptNodeParam0    gSpecialVar_0x8005
#define gSpecialVar_ScriptNodeParam1    gSpecialVar_0x8006

struct AdvPathConnectionSettings
{
    u8 minCount;
    u8 maxCount;
    u8 branchingChance[ROOM_CONNECTION_COUNT];
};

struct AdvPathGenerator
{
    struct AdvPathConnectionSettings connectionsSettingsPerColumn[MAX_CONNECTION_GENERATOR_COLUMNS];
};

struct AdvPathRoomSettings
{
    struct Coords8 currentCoords;
    struct RogueAdvPathRoomParams roomParams;
    u8 roomType;
};

struct AdvPathSettings
{
    const struct AdvPathGenerator* generator;
    struct AdvPathRoomSettings roomScratch[ROGUE_ADVPATH_ROOM_CAPACITY];
    u8 numOfRooms[ADVPATH_ROOM_COUNT];
    struct Coords8 currentCoords;
    u8 totalLength;
    u8 nodeCount;
};


struct MetatileOffset
{
    s16 x;
    s16 y;
    u32 metatile;
};

struct MetatileConnection
{
    u16 centre;
    u16 left;
    u16 right;
    u16 up;
    u16 down;
};

static const struct MetatileOffset sTreeDecorationMetatiles[] = 
{
    { 0, 0, METATILE_GeneralHub_Tree_BottomLeft_Sparse },
    { 1, 0, METATILE_GeneralHub_Tree_BottomRight_Sparse },
    { 0, -1, METATILE_GeneralHub_Tree_TopLeft_Sparse },
    { 1, -1, METATILE_GeneralHub_Tree_TopRight_Sparse },
    { 0, -2, METATILE_GeneralHub_Tree_TopLeft_CapGrass },
    { 1, -2, METATILE_GeneralHub_Tree_TopRight_CapGrass },
};

static bool8 IsObjectEventVisible(struct RogueAdvPathRoom* room);
static bool8 ShouldBlockObjectEvent(struct RogueAdvPathRoom* room);
static void BufferTypeAdjective(u8 type);

static void GeneratePath(struct AdvPathSettings* pathSettings);
static void GenerateFloorLayout(struct Coords8 currentCoords, struct AdvPathSettings* pathSettings);
static void GenerateRoomPlacements(struct AdvPathSettings* pathSettings);
static void GenerateRoomInstance(u8 roomId, u8 roomType);
static u8 CountRoomConnections(u8 mask);

static u8 GenerateRoomConnectionMask(struct Coords8 coords, struct AdvPathSettings* pathSettings);
static bool8 DoesRoomExists(s8 x, s8 y);

static u16 SelectObjectGfxForRoom(struct RogueAdvPathRoom* room);
static u8 SelectObjectMovementTypeForRoom(struct RogueAdvPathRoom* room);


static void GeneratePath(struct AdvPathSettings* pathSettings)
{
    struct AdvPathRoomSettings* bossRoom = &pathSettings->roomScratch[0];
    memset(bossRoom, 0, sizeof(bossRoom));

    AGB_ASSERT(pathSettings->generator != NULL);

    bossRoom->roomType = ADVPATH_ROOM_BOSS;

    // Generate base layout
    {
        struct Coords8 coords;
        coords.x = 0;
        coords.y = 0;

        gRogueAdvPath.roomCount = 0;
        gRogueAdvPath.pathLength = pathSettings->totalLength;

        GenerateFloorLayout(coords, pathSettings);
        GenerateRoomPlacements(pathSettings);
    }

    // Store min/max Y coords
    {
        u8 i;

        for(i = 0; i < gRogueAdvPath.roomCount; ++i)
        {
            if(i == 0)
            {
                gRogueAdvPath.pathMinY = gRogueAdvPath.rooms[i].coords.y;
                gRogueAdvPath.pathMaxY = gRogueAdvPath.rooms[i].coords.y;
            }
            else
            {
                gRogueAdvPath.pathMinY = min(gRogueAdvPath.pathMinY, gRogueAdvPath.rooms[i].coords.y);
                gRogueAdvPath.pathMaxY = max(gRogueAdvPath.pathMaxY, gRogueAdvPath.rooms[i].coords.y);
            }
        }
    }
}

static void GenerateFloorLayout(struct Coords8 currentCoords, struct AdvPathSettings* pathSettings)
{
    if(pathSettings->nodeCount >= ROGUE_ADVPATH_ROOM_CAPACITY)
    {
        // Cannot generate any more
        DebugPrint("ADVPATH: \tReached room/node capacity.");
        return;
    }
    else
    {
        u8 nodeId = gRogueAdvPath.roomCount++;

        // Write base settings for this room (These will likely be overriden later)
        gRogueAdvPath.rooms[nodeId].coords = currentCoords;
        gRogueAdvPath.rooms[nodeId].roomType = ADVPATH_ROOM_NONE;
        gRogueAdvPath.rooms[nodeId].connectionMask = 0;
        gRogueAdvPath.rooms[nodeId].rngSeed = RogueRandom();

        
        // Generate children
        //
        if(currentCoords.x + 1 < pathSettings->totalLength)
        {
            struct Coords8 newCoords;
            u8 connectionMask;

            newCoords.x = currentCoords.x + 1;
            newCoords.y = currentCoords.y;

            connectionMask = GenerateRoomConnectionMask(currentCoords, pathSettings);
            gRogueAdvPath.rooms[nodeId].connectionMask = connectionMask;

            newCoords.y = currentCoords.y + 1;
            if((connectionMask & ROOM_CONNECTION_MASK_TOP) != 0 && !DoesRoomExists(newCoords.x, newCoords.y))
            {
                GenerateFloorLayout(newCoords, pathSettings);
            }
            
            newCoords.y = currentCoords.y + 0;
            if((connectionMask & ROOM_CONNECTION_MASK_MID) != 0 && !DoesRoomExists(newCoords.x, newCoords.y))
            {
                GenerateFloorLayout(newCoords, pathSettings);
            }

            newCoords.y = currentCoords.y - 1;
            if((connectionMask & ROOM_CONNECTION_MASK_BOT) != 0 && !DoesRoomExists(newCoords.x, newCoords.y))
            {
                GenerateFloorLayout(newCoords, pathSettings);
            }
        }
    }
}

static bool8 IsPrecededByRoomType(struct RogueAdvPathRoom* room, u8 roomType)
{
    u8 i;

    for(i = 0; i < gRogueAdvPath.roomCount; ++i)
    {
        if(gRogueAdvPath.rooms[i].coords.x == room->coords.x + 1)
        {
            // ROOM_CONNECTION_MASK_TOP
            if((room->connectionMask & ROOM_CONNECTION_MASK_TOP) != 0 && gRogueAdvPath.rooms[i].coords.y == room->coords.y + 1)
            {
                if(gRogueAdvPath.rooms[i].roomType == roomType)
                    return TRUE;
            }
            // ROOM_CONNECTION_MASK_MID
            else if((room->connectionMask & ROOM_CONNECTION_MASK_MID) != 0 && gRogueAdvPath.rooms[i].coords.y == room->coords.y + 0)
            {
                if(gRogueAdvPath.rooms[i].roomType == roomType)
                    return TRUE;
            }
            // ROOM_CONNECTION_MASK_BOT
            else if((room->connectionMask & ROOM_CONNECTION_MASK_BOT) != 0 && gRogueAdvPath.rooms[i].coords.y == room->coords.y - 1)
            {
                if(gRogueAdvPath.rooms[i].roomType == roomType)
                    return TRUE;
            }
        }
    }

    return FALSE;
}

static bool8 IsProceededByRoomType(struct RogueAdvPathRoom* room, u8 roomType)
{
    u8 i;

    if(room->coords.x == 0)
        return FALSE;
    else if(room->coords.x == 1)
        return roomType == ADVPATH_ROOM_BOSS;

    // Check the inverse mask to see if we are connected
    for(i = 0; i < gRogueAdvPath.roomCount; ++i)
    {
        if(gRogueAdvPath.rooms[i].coords.x == room->coords.x - 1)
        {
            // ROOM_CONNECTION_MASK_TOP
            if((gRogueAdvPath.rooms[i].connectionMask & ROOM_CONNECTION_MASK_BOT) != 0 && gRogueAdvPath.rooms[i].coords.y == room->coords.y + 1)
            {
                if(gRogueAdvPath.rooms[i].roomType == roomType)
                    return TRUE;
            }
            // ROOM_CONNECTION_MASK_MID
            else if((gRogueAdvPath.rooms[i].connectionMask & ROOM_CONNECTION_MASK_MID) != 0 && gRogueAdvPath.rooms[i].coords.y == room->coords.y + 0)
            {
                if(gRogueAdvPath.rooms[i].roomType == roomType)
                    return TRUE;
            }
            // ROOM_CONNECTION_MASK_BOT
            else if((gRogueAdvPath.rooms[i].connectionMask & ROOM_CONNECTION_MASK_TOP) != 0 && gRogueAdvPath.rooms[i].coords.y == room->coords.y - 1)
            {
                if(gRogueAdvPath.rooms[i].roomType == roomType)
                    return TRUE;
            }
        }
    }

    return FALSE;
}

static u8 ReplaceRoomEncounters_CalculateWeight(u16 weightIndex, u16 roomId, void* data)
{
    s16 weight = 10;
    u8 roomType = *((u8*)data);
    struct RogueAdvPathRoom* existingRoom = &gRogueAdvPath.rooms[roomId];

    switch (roomType)
    {
    case ADVPATH_ROOM_RESTSTOP:
        // Like being placed in the final column but can occasionally end up in other one
        if(existingRoom->coords.x <= 2)
            weight += 80;

        // Don't place after or before or other rest stop
        if(IsPrecededByRoomType(existingRoom, ADVPATH_ROOM_RESTSTOP) || IsProceededByRoomType(existingRoom, ADVPATH_ROOM_RESTSTOP))
            weight = 0;
   
        if(IsPrecededByRoomType(existingRoom, ADVPATH_ROOM_LEGENDARY) || IsProceededByRoomType(existingRoom, ADVPATH_ROOM_LEGENDARY))
            weight = 0;
        break;

    case ADVPATH_ROOM_LEGENDARY:
        // Like being placed in the final column but can occasionally end up in other one
        if(existingRoom->coords.x <= 2)
            weight += 80;

        // Don't want to place in first column
        if(existingRoom->coords.x + 1 == gRogueAdvPath.pathLength)
            weight -= 40;

        // Prefer route where we are locked into this path
        if(CountRoomConnections(existingRoom->connectionMask) == 1)
            weight += 40;
        break;
    }

    // If we've got this encounter immediately before or after prefer not this one
    if(IsPrecededByRoomType(existingRoom, roomType))
        weight -= 5;
    if(IsProceededByRoomType(existingRoom, roomType))
        weight -= 5;


    return (u8)(min(255, max(0, weight)));
}

static void ReplaceRoomEncounters(u8 fromRoomType, u8 toRoomType, u8 placeCount)
{
    if(placeCount == 0)
        return;

    RoguePathsQuery_Begin();
    RoguePathsQuery_Reset(QUERY_FUNC_INCLUDE);
    RoguePathsQuery_IsOfType(QUERY_FUNC_INCLUDE, fromRoomType);

    RogueWeightQuery_Begin();
    {
        u8 i;
        u16 index;

        for(i = 0; i < placeCount; ++i)
        {
            RogueWeightQuery_CalculateWeights(ReplaceRoomEncounters_CalculateWeight, &toRoomType);

            if(!RogueWeightQuery_HasAnyWeights())
                break;
            
            index = RogueWeightQuery_SelectRandomFromWeights(RogueRandom());
            RogueMiscQuery_EditElement(QUERY_FUNC_EXCLUDE, index);

            GenerateRoomInstance(index, toRoomType);
        }
    }
    RogueWeightQuery_End();

    RoguePathsQuery_End();
}

static void GenerateRoomPlacements(struct AdvPathSettings* pathSettings)
{
    u8 i;
    u8 amount;
    bool8 isSmallFloor = gRogueAdvPath.roomCount <= 9;

    // Place gym at very end
    GenerateRoomInstance(0, ADVPATH_ROOM_BOSS);

    // Place routes on all tiles for now, so other encounters can choose to replace them
    for(i = 0; i < gRogueAdvPath.roomCount; ++i)
    {
        // Don't place them immediately before the gym
        if(gRogueAdvPath.rooms[i].coords.x > 1)
            GenerateRoomInstance(i, ADVPATH_ROOM_ROUTE);
    }

    // Now we're going to replace the routes based on the ideal placement
    // The order of these is important to decide the placement

    // Randomly replace a routes with empty tiles
    {
        u8 chance = 0;

        if(gRogueRun.currentDifficulty >=  ROGUE_CHAMP_START_DIFFICULTY)
        {
            chance = 90;
        }
        else if(gRogueRun.currentDifficulty >=  ROGUE_ELITE_START_DIFFICULTY)
        {
            chance = 50;
        }
        else if(gRogueRun.currentDifficulty >=  1)
        {
            chance = 5;
        }

        for(i = 0; i < gRogueAdvPath.roomCount; ++i)
        {
            if(gRogueAdvPath.rooms[i].roomType == ADVPATH_ROOM_ROUTE && RogueRandomChance(chance, 0))
                GenerateRoomInstance(i, ADVPATH_ROOM_NONE);
        }
    }

    // Legends
    // TODO - Legendary points
    {
        amount = 1;

        for(i = 0; i < ADVPATH_LEGEND_COUNT; ++i)
        {
            if(gRogueRun.legendarySpecies[i] != SPECIES_NONE && gRogueRun.legendaryDifficulties[i] == gRogueRun.currentDifficulty)
            {
                ReplaceRoomEncounters(ADVPATH_ROOM_ROUTE, ADVPATH_ROOM_LEGENDARY, amount);
                break;
            }
        }
    }

    // Rest stops
    // Place a max of 3 rest stops
    amount = isSmallFloor ? (0 + RogueRandom() % 2) : (2 + RogueRandom() % 2);
    ReplaceRoomEncounters(ADVPATH_ROOM_ROUTE, ADVPATH_ROOM_RESTSTOP, amount);

    // Lab
    amount = 0;
    if(gRogueRun.currentDifficulty >= ROGUE_GYM_MID_DIFFICULTY - 1 && RogueRandomChance(10, 0))
        amount = 1;
    ReplaceRoomEncounters(ADVPATH_ROOM_ROUTE, ADVPATH_ROOM_LAB, amount);


    // Miniboss
    // REMOVED

    // Dark deal / Game show
    if(!isSmallFloor)
    {
        bool8 allowDarkDeal = FALSE;
        bool8 allowGameShow = FALSE;

        if(gRogueRun.currentDifficulty >= ROGUE_GYM_MID_DIFFICULTY + 2)
        {
            // Only dark deals
            allowDarkDeal = TRUE;
        }
        else if(gRogueRun.currentDifficulty >= ROGUE_GYM_MID_DIFFICULTY - 2)
        {
            // Mix of both
            allowDarkDeal = TRUE;
            allowGameShow = TRUE;
        }
        else
        {
            // Only game show
            allowGameShow = TRUE;
        }

        if(allowDarkDeal)
        {
            u8 chance = 5;

            // Every 3rd difficulty have a chance
            if((gRogueRun.currentDifficulty % 3) == 0)
                chance = 50;

            if(RogueRandomChance(chance, 0))
            {
                amount = 1;
                ReplaceRoomEncounters(ADVPATH_ROOM_ROUTE, ADVPATH_ROOM_DARK_DEAL, amount);
            }
        }

        if(allowGameShow)
        {
            u8 chance = 50;

            // Inverted chance of dark deal
            if((gRogueRun.currentDifficulty % 3) == 0)
                chance = 10;

            if(RogueRandomChance(chance, 0))
            {
                amount = 1;
                ReplaceRoomEncounters(ADVPATH_ROOM_ROUTE, ADVPATH_ROOM_GAMESHOW, amount);
            }
        }
    }

    // Wild dens
    {
        u8 chance = 10;

        if(gRogueRun.currentDifficulty > ROGUE_ELITE_START_DIFFICULTY)
        {
            chance = 60;
        }

        for(i = 0; i < gRogueAdvPath.roomCount; ++i)
        {
            if(gRogueAdvPath.rooms[i].roomType == ADVPATH_ROOM_ROUTE && RogueRandomChance(chance, 0))
                GenerateRoomInstance(i, ADVPATH_ROOM_WILD_DEN);
        }
    }
}

static void GenerateRoomInstance(u8 roomId, u8 roomType)
{
    u16 weights[ADVPATH_SUBROOM_WEIGHT_COUNT];
    memset(weights, 0, sizeof(weights));

    // Count other
    //++pathSettings->numOfRooms[roomSettings->roomType];
    //DebugPrintf("ADVPATH: \tAdded room type %d (Total: %d)", roomType, pathSettings->numOfRooms[roomSettings->roomType]);

    // Erase any previously set params
    gRogueAdvPath.rooms[roomId].roomType = roomType;
    memset(&gRogueAdvPath.rooms[roomId].roomParams, 0, sizeof(gRogueAdvPath.rooms[roomId].roomParams));

    switch(roomType)
    {
        case ADVPATH_ROOM_BOSS:
            AGB_ASSERT(gRogueRun.currentDifficulty < ARRAY_COUNT(gRogueRun.bossTrainerNums));
            gRogueAdvPath.rooms[roomId].roomParams.perType.boss.trainerNum = gRogueRun.bossTrainerNums[gRogueRun.currentDifficulty];
            break;

        case ADVPATH_ROOM_RESTSTOP:
            weights[ADVPATH_SUBROOM_RESTSTOP_BATTLE] = 4;
            weights[ADVPATH_SUBROOM_RESTSTOP_SHOP] = 4;
            weights[ADVPATH_SUBROOM_RESTSTOP_FULL] = 1;

            gRogueAdvPath.rooms[roomId].roomParams.roomIdx = SelectIndexFromWeights(weights, ARRAY_COUNT(weights), RogueRandom());
            break;

        case ADVPATH_ROOM_LEGENDARY:
            {
                u8 legendId = Rogue_GetCurrentLegendaryEncounterId();
                u16 species = gRogueRun.legendarySpecies[legendId];
                gRogueAdvPath.rooms[roomId].roomParams.roomIdx = Rogue_GetLegendaryRoomForSpecies(species);
                gRogueAdvPath.rooms[roomId].roomParams.perType.legendary.shinyState = RogueRandomRange(Rogue_GetShinyOdds(), OVERWORLD_FLAG) == 0;
            }
            break;

        case ADVPATH_ROOM_MINIBOSS:
            gRogueAdvPath.rooms[roomId].roomParams.roomIdx = 0;
            gRogueAdvPath.rooms[roomId].roomParams.perType.miniboss.trainerNum = Rogue_NextMinibossTrainerId();
            break;

        case ADVPATH_ROOM_WILD_DEN:
            gRogueAdvPath.rooms[roomId].roomParams.roomIdx = 0;
            gRogueAdvPath.rooms[roomId].roomParams.perType.wildDen.species = Rogue_SelectWildDenEncounterRoom();
            gRogueAdvPath.rooms[roomId].roomParams.perType.wildDen.shinyState = RogueRandomRange(Rogue_GetShinyOdds(), OVERWORLD_FLAG) == 0;
            break;

        case ADVPATH_ROOM_ROUTE:
        {
            gRogueAdvPath.rooms[roomId].roomParams.roomIdx = Rogue_SelectRouteRoom();

            if(gRogueRun.currentDifficulty > ROGUE_ELITE_START_DIFFICULTY)
            {
                weights[ADVPATH_SUBROOM_ROUTE_CALM] = 0;
                weights[ADVPATH_SUBROOM_ROUTE_AVERAGE] = 1;
                weights[ADVPATH_SUBROOM_ROUTE_TOUGH] = 8;
            }
            else
            {
                weights[ADVPATH_SUBROOM_ROUTE_CALM] = 2;
                weights[ADVPATH_SUBROOM_ROUTE_AVERAGE] = 2;
                weights[ADVPATH_SUBROOM_ROUTE_TOUGH] = 1;
            }

            gRogueAdvPath.rooms[roomId].roomParams.perType.route.difficulty = SelectIndexFromWeights(weights, ARRAY_COUNT(weights), RogueRandom());
            break;
        }
    }
}

static u8 CountRoomConnections(u8 mask)
{
    u8 count = 0;

    if(mask == 0)
        return 0;

    if((mask & ROOM_CONNECTION_MASK_TOP) != 0)
        ++count;

    if((mask & ROOM_CONNECTION_MASK_MID) != 0)
        ++count;

    if((mask & ROOM_CONNECTION_MASK_BOT) != 0)
        ++count;

    return count;
}

static u8 GenerateRoomConnectionMask(struct Coords8 coords, struct AdvPathSettings* pathSettings)
{
    u8 mask, i;
    u8 connCount;
    u8 minConnCount = pathSettings->generator->connectionsSettingsPerColumn[min(coords.x, MAX_CONNECTION_GENERATOR_COLUMNS - 1)].minCount;
    u8 maxConnCount = pathSettings->generator->connectionsSettingsPerColumn[min(coords.x, MAX_CONNECTION_GENERATOR_COLUMNS - 1)].maxCount;
    u8 const* branchingChances = pathSettings->generator->connectionsSettingsPerColumn[min(coords.x, MAX_CONNECTION_GENERATOR_COLUMNS - 1)].branchingChance;

    do
    {
        mask = 0;

        if(RogueRandomChance(branchingChances[ROOM_CONNECTION_TOP], OVERWORLD_FLAG))
            mask |= ROOM_CONNECTION_MASK_TOP;

        if(RogueRandomChance(branchingChances[ROOM_CONNECTION_MID], OVERWORLD_FLAG))
            mask |= ROOM_CONNECTION_MASK_MID;

        if(RogueRandomChance(branchingChances[ROOM_CONNECTION_BOT], OVERWORLD_FLAG))
            mask |= ROOM_CONNECTION_MASK_BOT;

        connCount = CountRoomConnections(mask);
    }
    // keep going until we have the required number of connections
    while(!(connCount >= minConnCount && connCount <= maxConnCount));

    AGB_ASSERT(mask != 0);

    return mask;
}

static bool8 DoesRoomExists(s8 x, s8 y)
{
    u8 i;

    for(i = 0; i < gRogueAdvPath.roomCount; ++i)
    {
        if(gRogueAdvPath.rooms[i].coords.x == x && gRogueAdvPath.rooms[i].coords.y == y)
            return TRUE;
    }

    return FALSE;
}

bool8 RogueAdv_GenerateAdventurePathsIfRequired()
{
    if(gRogueRun.adventureRoomId != ADVPATH_INVALID_ROOM_ID && (gRogueAdvPath.roomCount != 0 && gRogueAdvPath.rooms[gRogueRun.adventureRoomId].roomType != ADVPATH_ROOM_BOSS))
    {
        // Path is still valid
        gRogueAdvPath.justGenerated = FALSE;
        return FALSE;
    }
    else
    {
        struct AdvPathSettings* pathSettings;
        struct AdvPathGenerator* generator;

        // If we have a valid room ID, then we're reloading a previous save
        bool8 isNewGeneration = gRogueRun.adventureRoomId == ADVPATH_INVALID_ROOM_ID;

        pathSettings = AllocZeroed(sizeof(struct AdvPathSettings));
        generator = AllocZeroed(sizeof(struct AdvPathGenerator));

        AGB_ASSERT(pathSettings != NULL);
        AGB_ASSERT(generator != NULL);

        pathSettings->generator = generator;
        pathSettings->totalLength = 3 + 2; // +2 to account for final encounter and initial split

        // Select the correct seed
        {
            u8 i;
            u16 seed;
            SeedRogueRng(gRogueRun.baseSeed * 235 + 31897);

            seed = RogueRandom();
            for(i = 0; i < gRogueRun.currentDifficulty; ++i)
            {
                seed = RogueRandom();
            }

            // This is the seed for this path
            SeedRogueRng(seed);
        }

        // Select some branching presets for the layout generation
        {
            u8 i;

            // Gym split
            generator->connectionsSettingsPerColumn[0].minCount = 2;
            generator->connectionsSettingsPerColumn[0].maxCount = 3;
            generator->connectionsSettingsPerColumn[0].branchingChance[ROOM_CONNECTION_TOP] = 33;
            generator->connectionsSettingsPerColumn[0].branchingChance[ROOM_CONNECTION_MID] = 33;
            generator->connectionsSettingsPerColumn[0].branchingChance[ROOM_CONNECTION_BOT] = 33;
            
            for(i = 1; i < MAX_CONNECTION_GENERATOR_COLUMNS; ++i)
            {
                // Random column variant switches
                switch (RogueRandom() % 4)
                {
                // Mixed/Standard
                case 0:
                    generator->connectionsSettingsPerColumn[i].minCount = 1;
                    generator->connectionsSettingsPerColumn[i].maxCount = 3;
                    generator->connectionsSettingsPerColumn[i].branchingChance[ROOM_CONNECTION_TOP] = 40;
                    generator->connectionsSettingsPerColumn[i].branchingChance[ROOM_CONNECTION_MID] = 40;
                    generator->connectionsSettingsPerColumn[i].branchingChance[ROOM_CONNECTION_BOT] = 40;
                    break;

                // Branches
                case 1:
                    generator->connectionsSettingsPerColumn[i].minCount = 1;
                    generator->connectionsSettingsPerColumn[i].maxCount = 2;
                    generator->connectionsSettingsPerColumn[i].branchingChance[ROOM_CONNECTION_TOP] = 40;
                    generator->connectionsSettingsPerColumn[i].branchingChance[ROOM_CONNECTION_MID] = 0;
                    generator->connectionsSettingsPerColumn[i].branchingChance[ROOM_CONNECTION_BOT] = 40;
                    break;

                // Lines
                case 2:
                    generator->connectionsSettingsPerColumn[i].minCount = 2;
                    generator->connectionsSettingsPerColumn[i].maxCount = 2;
                    generator->connectionsSettingsPerColumn[i].branchingChance[ROOM_CONNECTION_TOP] = 10;
                    generator->connectionsSettingsPerColumn[i].branchingChance[ROOM_CONNECTION_MID] = 50;
                    generator->connectionsSettingsPerColumn[i].branchingChance[ROOM_CONNECTION_BOT] = 10;
                    break;

                // Wiggling line
                case 3:
                    generator->connectionsSettingsPerColumn[i].minCount = 1;
                    generator->connectionsSettingsPerColumn[i].maxCount = 1;
                    generator->connectionsSettingsPerColumn[i].branchingChance[ROOM_CONNECTION_TOP] = 40;
                    generator->connectionsSettingsPerColumn[i].branchingChance[ROOM_CONNECTION_MID] = 0;
                    generator->connectionsSettingsPerColumn[i].branchingChance[ROOM_CONNECTION_BOT] = 40;
                    break;
                
                default:
                    AGB_ASSERT(FALSE);
                    break;
                }
            }
        }

        DebugPrintf("ADVPATH: Generating path for seed %d.", gRngRogueValue);
        Rogue_ResetAdventurePathBuffers();
        GeneratePath(pathSettings);
        DebugPrint("ADVPATH: Finished generating path.");

        Free(pathSettings);
        Free(generator);

        gRogueAdvPath.justGenerated = isNewGeneration;

        if(!isNewGeneration)
        {
            // Remember the room type/params
            gRogueAdvPath.currentRoomType = gRogueAdvPath.rooms[gRogueRun.adventureRoomId].roomType;
            memcpy(&gRogueAdvPath.currentRoomParams, &gRogueAdvPath.rooms[gRogueRun.adventureRoomId].roomParams, sizeof(gRogueAdvPath.currentRoomParams));
        }

        return isNewGeneration;
    }
}

void RogueAdv_ApplyAdventureMetatiles()
{
    struct Coords16 treesCoords[24];
    u32 metatile;
    u16 x, y;
    u16 treeCount;
    u8 i, j;
    bool8 isValid;
    u8 totalHeight;

    // Detect trees, as we will likely need to remove them later
    treeCount = 0;

    for(y = 0; y < gMapHeader.mapLayout->height; ++y)
    for(x = 0; x < gMapHeader.mapLayout->width; ++x)
    {
        metatile = MapGridGetMetatileIdAt(x + MAP_OFFSET, y + MAP_OFFSET);

        if(metatile == sTreeDecorationMetatiles[0].metatile)
        {
            treesCoords[treeCount].x = x;
            treesCoords[treeCount].y = y;
            ++treeCount;

            AGB_ASSERT(treeCount < ARRAY_COUNT(treesCoords));
        }
    }


    totalHeight = gRogueAdvPath.pathMaxY - gRogueAdvPath.pathMinY + 1;

    // Draw room path
    for(i = 0; i < gRogueAdvPath.roomCount; ++i)
    {
        // Move coords into world space
        x = ROOM_TO_METATILE_X(gRogueAdvPath.rooms[i].coords.x);
        y = ROOM_TO_METATILE_Y(gRogueAdvPath.rooms[i].coords.y);

        // Main tile where object will be placed
        //
        
        if(ShouldBlockObjectEvent(&gRogueAdvPath.rooms[i]))
        {
            // Place rock to block way back
            MapGridSetMetatileIdAt(x + 2, y, METATILE_General_SandPit_Stone | MAPGRID_COLLISION_MASK);
        }
        else
        {
            MapGridSetMetatileIdAt(x + 2, y, METATILE_General_SandPit_Center);
        }

        // Place connecting tiles infront
        //
        // ROOM_CONNECTION_MASK_MID (Always needed)
        MapGridSetMetatileIdAt(x + 1, y + 0, METATILE_General_SandPit_Center);
        
        if((gRogueAdvPath.rooms[i].connectionMask & ROOM_CONNECTION_MASK_TOP) != 0)
            MapGridSetMetatileIdAt(x + 1, y + 1, METATILE_General_SandPit_Center);

        if((gRogueAdvPath.rooms[i].connectionMask & ROOM_CONNECTION_MASK_BOT) != 0)
            MapGridSetMetatileIdAt(x + 1, y - 1, METATILE_General_SandPit_Center);

        // Place connecting tiles behind (Unless we're the final node)
        //
        if(i != 0)
        {
            for(j = 1; j < ROOM_TO_WORLD_X; ++j)
            {
                if(j == 1 && IsObjectEventVisible(&gRogueAdvPath.rooms[i]))
                    // Place stone to block interacting from the back
                    MapGridSetMetatileIdAt(x + 2 + j, y, METATILE_General_SandPit_Stone | MAPGRID_COLLISION_MASK);
                else
                    MapGridSetMetatileIdAt(x + 2 + j, y, METATILE_General_SandPit_Center);
            }
        }
    }

    // Draw initial start line
    {
        // find start/end coords
        u8 minY = (u8)-1;
        u8 maxY = 0;

        for(i = 0; i < gRogueAdvPath.roomCount; ++i)
        {
            // Count if in first column
            if(gRogueAdvPath.rooms[i].coords.x == gRogueAdvPath.pathLength - 1)
            {
                // Move coords into world space
                x = ROOM_TO_METATILE_X(gRogueAdvPath.rooms[i].coords.x);
                y = ROOM_TO_METATILE_Y(gRogueAdvPath.rooms[i].coords.y);

                minY = min(minY, y);
                maxY = max(maxY, y);
            }
        }

        for(i = minY; i <= maxY; ++i)
        {
            MapGridSetMetatileIdAt(x + 1, i, METATILE_General_SandPit_Center);
        }
    }

    // Remove any decorations that may have been split in parts by the path placement
    for(j = 0; j < treeCount; ++j)
    {
        x = treesCoords[j].x + MAP_OFFSET;
        y = treesCoords[j].y + MAP_OFFSET;

        // Check for any missing tiles
        isValid = TRUE;

        for(i = 0; i < ARRAY_COUNT(sTreeDecorationMetatiles); ++i)
        {
            if(MapGridGetMetatileIdAt(x + sTreeDecorationMetatiles[i].x, y + sTreeDecorationMetatiles[i].y) != sTreeDecorationMetatiles[i].metatile)
            {
                isValid = FALSE;
                break;
            }
        }

        // If we're missing a tile remove rest of the tree
        if(!isValid)
        {
            for(i = 0; i < ARRAY_COUNT(sTreeDecorationMetatiles); ++i)
            {
                if(MapGridGetMetatileIdAt(x + sTreeDecorationMetatiles[i].x, y + sTreeDecorationMetatiles[i].y) == sTreeDecorationMetatiles[i].metatile)
                {
                    MapGridSetMetatileIdAt(x + sTreeDecorationMetatiles[i].x, y + sTreeDecorationMetatiles[i].y, METATILE_General_Grass | MAPGRID_COLLISION_MASK);
                }
            }
        }
    }
}


static void SetBossRoomWarp(u16 trainerNum, struct WarpData* warp)
{
    if(gRogueRun.currentDifficulty < 8)
    {
        warp->mapGroup = MAP_GROUP(ROGUE_BOSS_0);
        warp->mapNum = MAP_NUM(ROGUE_BOSS_0);
    }
    else if(gRogueRun.currentDifficulty < 12)
    {
        Rogue_GetPreferredElite4Map(trainerNum, &warp->mapGroup, &warp->mapNum);
    }
    else if(gRogueRun.currentDifficulty < 13)
    {
        warp->mapGroup = MAP_GROUP(ROGUE_BOSS_12);
        warp->mapNum = MAP_NUM(ROGUE_BOSS_12);
    }
    else if(gRogueRun.currentDifficulty < 14)
    {
        warp->mapGroup = MAP_GROUP(ROGUE_BOSS_13);
        warp->mapNum = MAP_NUM(ROGUE_BOSS_13);
    }
}

static void ApplyCurrentNodeWarp(struct WarpData *warp)
{
    struct RogueAdvPathRoom* room = &gRogueAdvPath.rooms[gRogueRun.adventureRoomId];

    SeedRogueRng(room->rngSeed);

    switch(room->roomType)
    {
        case ADVPATH_ROOM_BOSS:
            SetBossRoomWarp(room->roomParams.perType.boss.trainerNum, warp);
            break;

        case ADVPATH_ROOM_RESTSTOP:
            warp->mapGroup = gRogueRestStopEncounterInfo.mapTable[room->roomParams.roomIdx].group;
            warp->mapNum = gRogueRestStopEncounterInfo.mapTable[room->roomParams.roomIdx].num;
            break;

        case ADVPATH_ROOM_ROUTE:
            warp->mapGroup = gRogueRouteTable.routes[room->roomParams.roomIdx].map.group;
            warp->mapNum = gRogueRouteTable.routes[room->roomParams.roomIdx].map.num;
            break;

        case ADVPATH_ROOM_LEGENDARY:
            warp->mapGroup = gRogueLegendaryEncounterInfo.mapTable[room->roomParams.roomIdx].group;
            warp->mapNum = gRogueLegendaryEncounterInfo.mapTable[room->roomParams.roomIdx].num;
            break;

        case ADVPATH_ROOM_MINIBOSS:
            warp->mapGroup = MAP_GROUP(ROGUE_ENCOUNTER_MINI_BOSS);
            warp->mapNum = MAP_NUM(ROGUE_ENCOUNTER_MINI_BOSS);
            break;

        case ADVPATH_ROOM_WILD_DEN:
            warp->mapGroup = MAP_GROUP(ROGUE_ENCOUNTER_DEN);
            warp->mapNum = MAP_NUM(ROGUE_ENCOUNTER_DEN);
            break;

        case ADVPATH_ROOM_GAMESHOW:
            warp->mapGroup = MAP_GROUP(ROGUE_ENCOUNTER_GAME_SHOW);
            warp->mapNum = MAP_NUM(ROGUE_ENCOUNTER_GAME_SHOW);
            break;

        case ADVPATH_ROOM_DARK_DEAL:
            warp->mapGroup = MAP_GROUP(ROGUE_ENCOUNTER_GRAVEYARD);
            warp->mapNum = MAP_NUM(ROGUE_ENCOUNTER_GRAVEYARD);
            break;

        case ADVPATH_ROOM_LAB:
            warp->mapGroup = MAP_GROUP(ROGUE_ENCOUNTER_LAB);
            warp->mapNum = MAP_NUM(ROGUE_ENCOUNTER_LAB);
            break;
    }
}

u8 RogueAdv_OverrideNextWarp(struct WarpData *warp)
{
    // Should already be set correctly for RogueAdv_WarpLastInteractedRoom
    if(!gRogueAdvPath.isOverviewActive)
    {
        bool8 freshPath = RogueAdv_GenerateAdventurePathsIfRequired();

        // Always jump back to overview screen, after a different route
        warp->mapGroup = MAP_GROUP(ROGUE_ADVENTURE_PATHS);
        warp->mapNum = MAP_NUM(ROGUE_ADVENTURE_PATHS);
        warp->warpId = WARP_ID_NONE;

        if(freshPath)
        {
            // Warp to initial start line
            // find start/end coords
            u8 i, x, y;
            u8 minY = (u8)-1;
            u8 maxY = 0;

            for(i = 0; i < gRogueAdvPath.roomCount; ++i)
            {
                // Count if in first column
                if(gRogueAdvPath.rooms[i].coords.x == gRogueAdvPath.pathLength - 1)
                {
                    // Move coords into world space
                    x = ROOM_TO_WARP_X(gRogueAdvPath.rooms[i].coords.x); 
                    y = ROOM_TO_WARP_Y(gRogueAdvPath.rooms[i].coords.y);

                    minY = min(minY, y);
                    maxY = max(maxY, y);
                }
            }

            warp->x = x - 2;
            warp->y = minY + (maxY - minY) / 2;
        }
        else
        {
            warp->x = ROOM_TO_WARP_X(gRogueAdvPath.rooms[gRogueRun.adventureRoomId].coords.x);
            warp->y = ROOM_TO_WARP_Y(gRogueAdvPath.rooms[gRogueRun.adventureRoomId].coords.y);
        }


        gRogueAdvPath.currentRoomType = ADVPATH_ROOM_NONE;
        return ROGUE_WARP_TO_ADVPATH;
    }
    else
    {
        ApplyCurrentNodeWarp(warp);
        return ROGUE_WARP_TO_ROOM;
    }
}

extern const u8 Rogue_AdventurePaths_InteractRoom[];

void RogueAdv_ModifyObjectEvents(struct MapHeader *mapHeader, struct ObjectEventTemplate *objectEvents, u8* objectEventCount, u8 objectEventCapacity)
{
    u8 i;
    u8 writeIdx;
    u8 x, y;
    u8 totalHeight;

    writeIdx = 0;
    totalHeight = gRogueAdvPath.pathMaxY - gRogueAdvPath.pathMinY + 1;

    // Draw room path
    for(i = 0; i < gRogueAdvPath.roomCount; ++i)
    {
        // Move coords into world space
        x = ROOM_TO_OBJECT_EVENT_X(gRogueAdvPath.rooms[i].coords.x);
        y = ROOM_TO_OBJECT_EVENT_Y(gRogueAdvPath.rooms[i].coords.y);

        if(writeIdx < objectEventCapacity)
        {
            if(IsObjectEventVisible(&gRogueAdvPath.rooms[i]))
            {
                objectEvents[writeIdx].localId = writeIdx;
                objectEvents[writeIdx].graphicsId = SelectObjectGfxForRoom(&gRogueAdvPath.rooms[i]);
                objectEvents[writeIdx].x = x;
                objectEvents[writeIdx].y = y;
                objectEvents[writeIdx].elevation = 3;
                objectEvents[writeIdx].trainerType = TRAINER_TYPE_NONE;
                objectEvents[writeIdx].movementType = SelectObjectMovementTypeForRoom(&gRogueAdvPath.rooms[i]);

                // Pack node into this var
                objectEvents[writeIdx].trainerRange_berryTreeId = i;
                objectEvents[writeIdx].script = Rogue_AdventurePaths_InteractRoom;

                ++writeIdx;
            }
        }
        else
        {
            DebugPrintf("WARNING: Cannot add adventure path object %d (out of range %d)", writeIdx, objectEventCapacity);
        }
    }

    *objectEventCount = writeIdx;
}

bool8 RogueAdv_CanUseEscapeRope(void)
{
    if(!gRogueAdvPath.isOverviewActive)
    {
        // We are in transition i.e. just started the run
        if(gRogueAdvPath.roomCount == 0)
            return FALSE;
        
        switch(gRogueAdvPath.currentRoomType)
        {
            case ADVPATH_ROOM_BOSS:
                return FALSE;

            default:
                return TRUE;
        }
    }

    return FALSE;
}

static u16 GetTypeForHint(struct RogueAdvPathRoom* room)
{
    return gRogueRouteTable.routes[room->roomParams.roomIdx].wildTypeTable[(room->coords.x + room->coords.y) % ARRAY_COUNT(gRogueRouteTable.routes[0].wildTypeTable)];
}

static u16 SelectObjectGfxForRoom(struct RogueAdvPathRoom* room)
{
    switch(room->roomType)
    {
        case ADVPATH_ROOM_NONE:
            return 0;
            
        case ADVPATH_ROOM_ROUTE:
        {
            switch(GetTypeForHint(room))
            {
                case TYPE_BUG:
                    return OBJ_EVENT_GFX_ROUTE_BUG;
                case TYPE_DARK:
                    return OBJ_EVENT_GFX_ROUTE_DARK;
                case TYPE_DRAGON:
                    return OBJ_EVENT_GFX_ROUTE_DRAGON;
                case TYPE_ELECTRIC:
                    return OBJ_EVENT_GFX_ROUTE_ELECTRIC;
#ifdef ROGUE_EXPANSION
                case TYPE_FAIRY:
                    return OBJ_EVENT_GFX_ROUTE_FAIRY;
#endif
                case TYPE_FIGHTING:
                    return OBJ_EVENT_GFX_ROUTE_FIGHTING;
                case TYPE_FIRE:
                    return OBJ_EVENT_GFX_ROUTE_FIRE;
                case TYPE_FLYING:
                    return OBJ_EVENT_GFX_ROUTE_FLYING;
                case TYPE_GHOST:
                    return OBJ_EVENT_GFX_ROUTE_GHOST;
                case TYPE_GRASS:
                    return OBJ_EVENT_GFX_ROUTE_GRASS;
                case TYPE_GROUND:
                    return OBJ_EVENT_GFX_ROUTE_GROUND;
                case TYPE_ICE:
                    return OBJ_EVENT_GFX_ROUTE_ICE;
                case TYPE_NORMAL:
                    return OBJ_EVENT_GFX_ROUTE_NORMAL;
                case TYPE_POISON:
                    return OBJ_EVENT_GFX_ROUTE_POISON;
                case TYPE_PSYCHIC:
                    return OBJ_EVENT_GFX_ROUTE_PSYCHIC;
                case TYPE_ROCK:
                    return OBJ_EVENT_GFX_ROUTE_ROCK;
                case TYPE_STEEL:
                    return OBJ_EVENT_GFX_ROUTE_STEEL;
                case TYPE_WATER:
                    return OBJ_EVENT_GFX_ROUTE_WATER;

                default:
                //case TYPE_MYSTERY:
                    return OBJ_EVENT_GFX_ROUTE_MYSTERY;
            }
        }

        case ADVPATH_ROOM_RESTSTOP:
            return gRogueRestStopEncounterInfo.mapTable[room->roomParams.roomIdx].encounterId;

        case ADVPATH_ROOM_LEGENDARY:
            return OBJ_EVENT_GFX_TRICK_HOUSE_STATUE;

        case ADVPATH_ROOM_MINIBOSS:
            return OBJ_EVENT_GFX_NOLAND;

        case ADVPATH_ROOM_WILD_DEN:
            return OBJ_EVENT_GFX_GRASS_CUSHION;

        case ADVPATH_ROOM_GAMESHOW:
            return OBJ_EVENT_GFX_CONTEST_JUDGE;

        case ADVPATH_ROOM_DARK_DEAL:
            return OBJ_EVENT_GFX_DEVIL_MAN;

        case ADVPATH_ROOM_LAB:
            return OBJ_EVENT_GFX_PC;

        case ADVPATH_ROOM_BOSS:
            return OBJ_EVENT_GFX_BALL_CUSHION; // ?
    }

    return 0;
}

static u8 SelectObjectMovementTypeForRoom(struct RogueAdvPathRoom* room)
{
    switch(room->roomType)
    {
        case ADVPATH_ROOM_ROUTE:
        {
            switch(room->roomParams.perType.route.difficulty)
            {
                case 1: // ADVPATH_SUBROOM_ROUTE_AVERAGE
                    return MOVEMENT_TYPE_FACE_UP;
                case 2: // ADVPATH_SUBROOM_ROUTE_TOUGH
                    return MOVEMENT_TYPE_FACE_LEFT;
                default: // ADVPATH_SUBROOM_ROUTE_CALM
                    return MOVEMENT_TYPE_NONE;
            };
        }
    }

    return MOVEMENT_TYPE_NONE;
}

static bool8 IsObjectEventVisible(struct RogueAdvPathRoom* room)
{
    if(room->roomType == ADVPATH_ROOM_NONE)
        return FALSE;

    if(gRogueAdvPath.justGenerated)
    {
        // Everything is visible
        return TRUE;
    }
    else
    {
        u8 focusX = gRogueAdvPath.rooms[gRogueRun.adventureRoomId].coords.x;
        return room->coords.x < focusX;
    }
}

static bool8 ShouldBlockObjectEvent(struct RogueAdvPathRoom* room)
{
    if(gRogueAdvPath.justGenerated)
    {
        return FALSE;
    }
    else
    {
        u8 focusX = gRogueAdvPath.rooms[gRogueRun.adventureRoomId].coords.x;
        return room->coords.x == focusX;
    }
}

static void BufferTypeAdjective(u8 type)
{
    const u8 gText_AdjNormal[] = _("TYPICAL");
    const u8 gText_AdjFighting[] = _("MIGHTY");
    const u8 gText_AdjFlying[] = _("BREEZY");
    const u8 gText_AdjPoison[] = _("CORROSIVE");
    const u8 gText_AdjGround[] = _("COARSE");
    const u8 gText_AdjRock[] = _("RUGGED");
    const u8 gText_AdjBug[] = _("SWARMING");
    const u8 gText_AdjGhost[] = _("SPOOKY");
    const u8 gText_AdjSteel[] = _("SHARP");
    const u8 gText_AdjFire[] = _("WARM");
    const u8 gText_AdjWater[] = _("WET");
    const u8 gText_AdjGrass[] = _("VERDANT");
    const u8 gText_AdjElectric[] = _("ENERGETIC");
    const u8 gText_AdjPsychic[] = _("CONFUSING");
    const u8 gText_AdjIce[] = _("CHILLY");
    const u8 gText_AdjDragon[] = _("FIERCE");
    const u8 gText_AdjDark[] = _("GLOOMY");
#ifdef ROGUE_EXPANSION
    const u8 gText_AdjFairy[] = _("MAGICAL");
#endif
    const u8 gText_AdjNone[] = _("???");

    switch(type)
    {
        case TYPE_NORMAL:
            StringCopy(gStringVar1, gText_AdjNormal);
            break;

        case TYPE_FIGHTING:
            StringCopy(gStringVar1, gText_AdjFighting);
            break;

        case TYPE_FLYING:
            StringCopy(gStringVar1, gText_AdjFlying);
            break;

        case TYPE_POISON:
            StringCopy(gStringVar1, gText_AdjPoison);
            break;

        case TYPE_GROUND:
            StringCopy(gStringVar1, gText_AdjGround);
            break;

        case TYPE_ROCK:
            StringCopy(gStringVar1, gText_AdjRock);
            break;

        case TYPE_BUG:
            StringCopy(gStringVar1, gText_AdjBug);
            break;

        case TYPE_GHOST:
            StringCopy(gStringVar1, gText_AdjGhost);
            break;

        case TYPE_STEEL:
            StringCopy(gStringVar1, gText_AdjSteel);
            break;

        case TYPE_FIRE:
            StringCopy(gStringVar1, gText_AdjFire);
            break;

        case TYPE_WATER:
            StringCopy(gStringVar1, gText_AdjWater);
            break;

        case TYPE_GRASS:
            StringCopy(gStringVar1, gText_AdjGrass);
            break;

        case TYPE_ELECTRIC:
            StringCopy(gStringVar1, gText_AdjElectric);
            break;

        case TYPE_PSYCHIC:
            StringCopy(gStringVar1, gText_AdjPsychic);
            break;

        case TYPE_ICE:
            StringCopy(gStringVar1, gText_AdjIce);
            break;

        case TYPE_DRAGON:
            StringCopy(gStringVar1, gText_AdjDragon);
            break;

        case TYPE_DARK:
            StringCopy(gStringVar1, gText_AdjDark);
            break;

#ifdef ROGUE_EXPANSION
        case TYPE_FAIRY:
            StringCopy(gStringVar1, gText_AdjFairy);
            break;
#endif

        default:
            StringCopy(gStringVar1, gText_AdjNone);
            break;
    }
}

static u8 GetRoomIndexFromLastInteracted()
{
    u16 lastTalkedId = VarGet(VAR_LAST_TALKED);

    // We have to lookup into the template as this var gets zeroed when it's not being used in a valid way
    return gSaveBlock1Ptr->objectEventTemplates[lastTalkedId].trainerRange_berryTreeId;

    //u8 objEventId = GetObjectEventIdByLocalIdAndMap(lastTalkedId, gSaveBlock1Ptr->location.mapNum, gSaveBlock1Ptr->location.mapGroup);
    //u8 roomIdx = gObjectEvents[objEventId].trainerRange_berryTreeId;
    //return roomIdx;
}

void RogueAdv_GetLastInteractedRoomParams()
{
    u8 roomIdx = GetRoomIndexFromLastInteracted();

    gSpecialVar_ScriptNodeParam0 = gRogueAdvPath.rooms[roomIdx].roomType;
    gSpecialVar_ScriptNodeParam1 = gRogueAdvPath.rooms[roomIdx].roomParams.roomIdx;

    switch(gRogueAdvPath.rooms[roomIdx].roomType)
    {
        case ADVPATH_ROOM_ROUTE:
            gSpecialVar_ScriptNodeParam1 = gRogueAdvPath.rooms[roomIdx].roomParams.perType.route.difficulty;
            BufferTypeAdjective(GetTypeForHint(&gRogueAdvPath.rooms[roomIdx]));
            break;
    }
}

void RogueAdv_WarpLastInteractedRoom()
{
    struct WarpData warp;
    u8 roomIdx = GetRoomIndexFromLastInteracted();

    // Move to the selected node
    gRogueRun.adventureRoomId = roomIdx;
    gRogueAdvPath.currentRoomType = gRogueAdvPath.rooms[roomIdx].roomType;
    memcpy(&gRogueAdvPath.currentRoomParams, &gRogueAdvPath.rooms[roomIdx].roomParams, sizeof(gRogueAdvPath.currentRoomParams));

    // Fill with dud warp
    warp.mapGroup = MAP_GROUP(ROGUE_HUB_TRANSITION);
    warp.mapNum = MAP_NUM(ROGUE_HUB_TRANSITION);
    warp.warpId = 0;
    warp.x = -1;
    warp.y = -1;

    SetWarpDestination(warp.mapGroup, warp.mapNum, warp.warpId, warp.x, warp.y);
    DoWarp();
    ResetInitialPlayerAvatarState();
}