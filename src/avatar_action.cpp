#include "avatar_action.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <ostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "action.h"
#include "activity_actor_definitions.h"
#include "avatar.h"
#include "bodypart.h"
#include "cached_options.h"
#include "calendar.h"
#include "character.h"
#include "construction.h"
#include "creature.h"
#include "creature_tracker.h"
#include "debug.h"
#include "enums.h"
#include "flag.h"
#include "game.h"
#include "game_constants.h"
#include "game_inventory.h"
#include "inventory.h"
#include "item.h"
#include "item_location.h"
#include "itype.h"
#include "line.h"
#include "map.h"
#include "map_iterator.h"
#include "mapdata.h"
#include "math_defines.h"
#include "memory_fast.h"
#include "messages.h"
#include "monster.h"
#include "move_mode.h"
#include "mtype.h"
#include "npc.h"
#include "options.h"
#include "output.h"
#include "pimpl.h"
#include "player_activity.h"
#include "projectile.h"
#include "ranged.h"
#include "ret_val.h"
#include "rng.h"
#include "translations.h"
#include "type_id.h"
#include "value_ptr.h"
#include "veh_type.h"
#include "vehicle.h"
#include "vpart_position.h"

class gun_mode;

static const construction_str_id construction_constr_deconstruct_simple
    = construction_str_id( "constr_deconstruct_simple" );
static const construction_str_id construction_constr_deconstruct( "constr_deconstruct" );

static const efftype_id effect_amigara( "amigara" );
static const efftype_id effect_glowing( "glowing" );
static const efftype_id effect_harnessed( "harnessed" );
static const efftype_id effect_hunger_engorged( "hunger_engorged" );
static const efftype_id effect_incorporeal( "incorporeal" );
static const efftype_id effect_onfire( "onfire" );
static const efftype_id effect_pet( "pet" );
static const efftype_id effect_ridden( "ridden" );
static const efftype_id effect_stunned( "stunned" );
static const efftype_id effect_winded( "winded" );

static const itype_id itype_swim_fins( "swim_fins" );

static const mon_flag_str_id mon_flag_IMMOBILE( "IMMOBILE" );
static const mon_flag_str_id mon_flag_RIDEABLE_MECH( "RIDEABLE_MECH" );

static const move_mode_id move_mode_prone( "prone" );

static const skill_id skill_swimming( "swimming" );

static const trait_id trait_GRAZER( "GRAZER" );
static const trait_id trait_RUMINANT( "RUMINANT" );
static const trait_id trait_SHELL2( "SHELL2" );
static const trait_id trait_SHELL3( "SHELL3" );

static const zone_type_id zone_type_BUMP_INTERACT( "BUMP_INTERACT" );

#define dbg(x) DebugLog((x),D_SDL) << __FILE__ << ":" << __LINE__ << ": "

static bool check_water_affect_items( avatar &you )
{
    if( you.has_effect( effect_stunned ) ) {
        return true;
    }

    std::vector<item_location> dissolved;
    std::vector<item_location> destroyed;
    std::vector<item_location> wet;

    for( item_location &loc : you.all_items_loc() ) {
        if( loc->has_flag( flag_WATER_DISSOLVE ) && !loc.protected_from_liquids() ) {
            dissolved.emplace_back( loc );
        } else if( loc->has_flag( flag_WATER_BREAK ) && !loc->is_broken()
                   && !loc.protected_from_liquids() ) {
            destroyed.emplace_back( loc );
        } else if( loc->has_flag( flag_WATER_BREAK_ACTIVE ) && !loc->is_broken()
                   && !loc.protected_from_liquids() ) {
            wet.emplace_back( loc );
        }
    }

    if( dissolved.empty() && destroyed.empty() && wet.empty() ) {
        return query_yn( _( "Dive into the water?" ) );
    }

    uilist menu;
    menu.title = _( "Diving will destroy the following items.  Proceed?" );
    menu.text = _( "These items are not inside a waterproof container." );

    menu.addentry( 0, true, 'N', _( "No" ) );
    menu.addentry( 1, true, 'Y', _( "Yes" ) );

    auto add_header = [&menu]( const std::string & str ) {
        menu.addentry( -1, false, -1, "" );
        uilist_entry header( -1, false, -1, str, c_yellow, c_yellow );
        header.force_color = true;
        menu.entries.push_back( header );
    };

    if( !dissolved.empty() ) {
        add_header( _( "Will be dissolved:" ) );
        for( item_location &it : dissolved ) {
            menu.addentry( -1, false, -1, it->display_name() );
        }
    }

    if( !destroyed.empty() ) {
        add_header( _( "Will be destroyed:" ) );
        for( item_location &it : destroyed ) {
            menu.addentry( -1, false, -1, it->display_name() );
        }
    }

    if( !wet.empty() ) {
        add_header( _( "Will get wet:" ) );
        for( item_location &it : wet ) {
            menu.addentry( -1, false, -1, it->display_name() );
        }
    }

    menu.query();
    if( menu.ret != 1 ) {
        you.add_msg_if_player( _( "You back away from the water." ) );
        return false;
    }

    return true;
}

bool avatar_action::move( avatar &you, map &m, const tripoint &d )
{
    bool in_shell = you.has_active_mutation( trait_SHELL2 ) ||
                    you.has_active_mutation( trait_SHELL3 );
    if( ( !g->check_safe_mode_allowed() ) || in_shell ) {
        if( in_shell ) {
            add_msg( m_warning, _( "You can't move while in your shell.  Deactivate it to go mobile." ) );
        }
        return false;
    }

    // If any leg broken without crutches and not already on the ground topple over
    if( ( !you.enough_working_legs() && !you.is_prone() &&
          !( you.get_wielded_item() && you.get_wielded_item()->has_flag( flag_CRUTCHES ) ) ) ) {
        you.set_movement_mode( move_mode_prone );
        you.add_msg_if_player( m_bad,
                               _( "Your broken legs can't hold your weight and you fall down in pain." ) );
    }

    const bool is_riding = you.is_mounted();
    tripoint dest_loc;
    if( d.z == 0 && you.has_effect( effect_stunned ) ) {
        dest_loc.x = rng( you.posx() - 1, you.posx() + 1 );
        dest_loc.y = rng( you.posy() - 1, you.posy() + 1 );
        dest_loc.z = you.posz();
    } else {
        dest_loc.x = you.posx() + d.x;
        dest_loc.y = you.posy() + d.y;
        dest_loc.z = you.posz() + d.z;
    }

    if( dest_loc == you.pos() ) {
        // Well that sure was easy
        return true;
    }
    bool via_ramp = false;
    if( m.has_flag( ter_furn_flag::TFLAG_RAMP_UP, dest_loc ) ) {
        dest_loc.z += 1;
        via_ramp = true;
    } else if( m.has_flag( ter_furn_flag::TFLAG_RAMP_DOWN, dest_loc ) ) {
        dest_loc.z -= 1;
        via_ramp = true;
    }

    item_location weapon = you.get_wielded_item();
    if( m.has_flag( ter_furn_flag::TFLAG_MINEABLE, dest_loc ) && g->mostseen == 0 &&
        get_option<bool>( "AUTO_FEATURES" ) && get_option<bool>( "AUTO_MINING" ) &&
        !m.veh_at( dest_loc ) && !you.is_underwater() && !you.has_effect( effect_stunned ) &&
        !is_riding && !you.has_effect( effect_incorporeal ) ) {
        if( weapon && weapon->has_flag( flag_DIG_TOOL ) ) {
            if( weapon->type->can_use( "JACKHAMMER" ) &&
                weapon->ammo_sufficient( &you ) ) {
                you.invoke_item( &*weapon, "JACKHAMMER", dest_loc );
                // don't move into the tile until done mining
                you.defer_move( dest_loc );
                return true;
            } else if( weapon->type->can_use( "PICKAXE" ) ) {
                you.invoke_item( &*weapon, "PICKAXE", dest_loc );
                // don't move into the tile until done mining
                you.defer_move( dest_loc );
                return true;
            }
        }
    }

    // by this point we're either walking, running, crouching, or attacking, so update the activity level to match
    if( !is_riding ) {
        you.set_activity_level( you.current_movement_mode()->exertion_level() );
    }

    // If the player is *attempting to* move on the X axis, update facing direction of their sprite to match.
    point new_d( dest_loc.xy() + point( -you.posx(), -you.posy() ) );

    if( !g->is_tileset_isometric() ) {
        if( new_d.x > 0 ) {
            you.facing = FacingDirection::RIGHT;
            if( is_riding ) {
                you.mounted_creature->facing = FacingDirection::RIGHT;
            }
        } else if( new_d.x < 0 ) {
            you.facing = FacingDirection::LEFT;
            if( is_riding ) {
                you.mounted_creature->facing = FacingDirection::LEFT;
            }
        }
    } else {
        //
        // iso:
        //
        // right key            =>  +x -y       FacingDirection::RIGHT
        // left key             =>  -x +y       FacingDirection::LEFT
        // up key               =>  +x +y       ______
        // down key             =>  -x -y       ______
        // y: left-up key       =>  __ +y       FacingDirection::LEFT
        // u: right-up key      =>  +x __       FacingDirection::RIGHT
        // b: left-down key     =>  -x __       FacingDirection::LEFT
        // n: right-down key    =>  __ -y       FacingDirection::RIGHT
        //
        // right key            =>  +x -y       FacingDirection::RIGHT
        // u: right-up key      =>  +x __       FacingDirection::RIGHT
        // n: right-down key    =>  __ -y       FacingDirection::RIGHT
        // up key               =>  +x +y       ______
        // down key             =>  -x -y       ______
        // left key             =>  -x +y       FacingDirection::LEFT
        // y: left-up key       =>  __ +y       FacingDirection::LEFT
        // b: left-down key     =>  -x __       FacingDirection::LEFT
        //
        // right key            =>  +x +y       FacingDirection::RIGHT
        // u: right-up key      =>  +x __       FacingDirection::RIGHT
        // n: right-down key    =>  __ +y       FacingDirection::RIGHT
        // up key               =>  +x -y       ______
        // left key             =>  -x -y       FacingDirection::LEFT
        // b: left-down key     =>  -x __       FacingDirection::LEFT
        // y: left-up key       =>  __ -y       FacingDirection::LEFT
        // down key             =>  -x +y       ______
        //
        if( new_d.x >= 0 && new_d.y >= 0 ) {
            you.facing = FacingDirection::RIGHT;
            if( is_riding ) {
                auto *mons = you.mounted_creature.get();
                mons->facing = FacingDirection::RIGHT;
            }
        }
        if( new_d.y <= 0 && new_d.x <= 0 ) {
            you.facing = FacingDirection::LEFT;
            if( is_riding ) {
                auto *mons = you.mounted_creature.get();
                mons->facing = FacingDirection::LEFT;
            }
        }
    }

    if( you.has_effect( effect_amigara ) ) {
        int curdist = INT_MAX;
        int newdist = INT_MAX;
        const tripoint minp = tripoint( 0, 0, you.posz() );
        const tripoint maxp = tripoint( MAPSIZE_X, MAPSIZE_Y, you.posz() );
        for( const tripoint &pt : m.points_in_rectangle( minp, maxp ) ) {
            if( m.ter( pt ) == t_fault ) {
                int dist = rl_dist( pt, you.pos() );
                if( dist < curdist ) {
                    curdist = dist;
                }
                dist = rl_dist( pt, dest_loc );
                if( dist < newdist ) {
                    newdist = dist;
                }
            }
        }
        if( newdist > curdist ) {
            add_msg( m_info, _( "You cannot pull yourself away from the faultline…" ) );
            return false;
        }
    }

    dbg( D_PEDANTIC_INFO ) << "game:plmove: From (" <<
                           you.posx() << "," << you.posy() << "," << you.posz() << ") to (" <<
                           dest_loc.x << "," << dest_loc.y << "," << dest_loc.z << ")";

    if( g->disable_robot( dest_loc ) ) {
        return false;
    }

    // Check if our movement is actually an attack on a monster or npc
    // Are we displacing a monster?

    creature_tracker &creatures = get_creature_tracker();
    bool attacking = false;
    if( creatures.creature_at( dest_loc ) ) {
        attacking = true;
    }

    if( !you.move_effects( attacking ) ) {
        you.moves -= 100;
        return false;
    }

    if( monster *const mon_ptr = creatures.creature_at<monster>( dest_loc, true ) ) {
        monster &critter = *mon_ptr;
        if( critter.friendly == 0 &&
            !critter.has_effect( effect_pet ) ) {
            if( you.is_auto_moving() ) {
                add_msg( m_warning, _( "Monster in the way.  Auto move canceled." ) );
                add_msg( m_info, _( "Move into the monster to attack." ) );
                you.clear_destination();
                return false;
            }
            if( !you.try_break_relax_gas( _( "Your willpower asserts itself, and so do you!" ),
                                          _( "You're too pacified to strike anything…" ) ) ) {
                return false;
            }
            bool safe_mode = ( get_option<bool>( "SAFEMODE" ) ? SAFE_MODE_ON : SAFE_MODE_OFF );
            if( safe_mode ) {
                // If safe mode is enabled, only allow attacking neutral creatures when it is inactive
                if( critter.attitude_to( you ) == Creature::Attitude::NEUTRAL &&
                    g->safe_mode != SAFE_MODE_OFF ) {
                    const std::string msg_safe_mode = press_x( ACTION_TOGGLE_SAFEMODE );
                    add_msg( m_warning,
                             _( "Not attacking the %1$s -- safe mode is on!  (%2$s to turn it off)" ), critter.name(),
                             msg_safe_mode );
                    return false;
                }
            } else {
                // If safe mode is disabled, ask for confirmation before attacking a neutral creature
                if( critter.attitude_to( you ) == Creature::Attitude::NEUTRAL &&
                    !query_yn( _( "You may be attacked!  Proceed?" ) ) ) {
                    return false;
                }
            }
            you.melee_attack( critter, true );
            if( critter.is_hallucination() ) {
                critter.die( &you );
            }
            g->draw_hit_mon( dest_loc, critter, critter.is_dead() );
            return false;
        } else if( critter.has_flag( mon_flag_IMMOBILE ) || critter.has_effect( effect_harnessed ) ||
                   critter.has_effect( effect_ridden ) ) {
            add_msg( m_info, _( "You can't displace your %s." ), critter.name() );
            return false;
        }
        // Successful displacing is handled (much) later
    }
    // If not a monster, maybe there's an NPC there
    if( npc *const np_ = creatures.creature_at<npc>( dest_loc ) ) {
        npc &np = *np_;
        if( you.is_auto_moving() ) {
            add_msg( _( "NPC in the way, Auto move canceled." ) );
            add_msg( m_info, _( "Move into the NPC to interact or attack." ) );
            you.clear_destination();
            return false;
        }

        if( !np.is_enemy() ) {
            g->npc_menu( np );
            return false;
        }

        you.melee_attack( np, true );
        np.make_angry();
        return false;
    }

    // GRAB: pre-action checking.
    int dpart = -1;
    const optional_vpart_position vp0 = m.veh_at( you.pos() );
    vehicle *const veh0 = veh_pointer_or_null( vp0 );
    const optional_vpart_position vp1 = m.veh_at( dest_loc );
    vehicle *const veh1 = veh_pointer_or_null( vp1 );

    bool veh_closed_door = false;
    bool outside_vehicle = veh0 == nullptr || veh0 != veh1;
    if( veh1 != nullptr ) {
        dpart = veh1->next_part_to_open( vp1->part_index(), outside_vehicle );
        veh_closed_door = dpart >= 0 && !veh1->part( dpart ).open;
    }

    if( veh0 != nullptr && std::abs( veh0->velocity ) > 100 ) {
        if( veh1 == nullptr ) {
            if( query_yn( _( "Dive from moving vehicle?" ) ) ) {
                g->moving_vehicle_dismount( dest_loc );
            }
            return false;
        } else if( veh1 != veh0 ) {
            add_msg( m_info, _( "There is another vehicle in the way." ) );
            return false;
        } else if( !vp1.part_with_feature( "BOARDABLE", true ) ) {
            add_msg( m_info, _( "That part of the vehicle is currently unsafe." ) );
            return false;
        }
    }
    bool toSwimmable = m.has_flag( ter_furn_flag::TFLAG_SWIMMABLE, dest_loc ) &&
                       !m.has_flag_furn( "BRIDGE", dest_loc );
    bool toDeepWater = m.has_flag( ter_furn_flag::TFLAG_DEEP_WATER, dest_loc ) &&
                       !m.has_flag_furn( "BRIDGE", dest_loc );
    bool fromSwimmable = m.has_flag( ter_furn_flag::TFLAG_SWIMMABLE, you.pos() );
    bool fromDeepWater = m.has_flag( ter_furn_flag::TFLAG_DEEP_WATER, you.pos() );
    bool fromBoat = veh0 != nullptr;
    bool toBoat = veh1 != nullptr;
    if( is_riding ) {
        if( !you.check_mount_will_move( dest_loc ) ) {
            if( you.is_auto_moving() ) {
                you.clear_destination();
            }
            you.moves -= 20;
            return false;
        }
    }
    // Dive into water!
    if( toSwimmable && toDeepWater && !toBoat ) {
        // Requires confirmation if we were on dry land previously
        if( is_riding ) {
            auto *mon = you.mounted_creature.get();
            if( !mon->swims() || mon->get_size() < you.get_size() + 2 ) {
                add_msg( m_warning, _( "The %s cannot swim while it is carrying you!" ), mon->get_name() );
                return false;
            }
        }
        if( ( fromSwimmable && fromDeepWater && !fromBoat ) ||
            check_water_affect_items( you ) ) {
            if( ( !fromDeepWater || fromBoat ) && you.swim_speed() < 500 ) {
                add_msg( _( "You start swimming." ) );
                add_msg( m_info, _( "%s to dive underwater." ),
                         press_x( ACTION_MOVE_DOWN ) );
            }
            avatar_action::swim( get_map(), get_avatar(), dest_loc );
        }

        g->on_move_effects();
        return true;
    }

    //Wooden Fence Gate (or equivalently walkable doors):
    // open it if we are walking
    // vault over it if we are running
    std::string door_name = m.obstacle_name( dest_loc );
    if( m.passable_ter_furn( dest_loc )
        && you.is_walking()
        && !veh_closed_door
        && m.open_door( you, dest_loc, !m.is_outside( you.pos() ) ) ) {
        you.moves -= 100;
        you.add_msg_if_player( _( "You open the %s." ), door_name );
        // if auto move is on, continue moving next turn
        if( you.is_auto_moving() ) {
            you.defer_move( dest_loc );
        }
        return true;
    }
    if( int move_cost = m.move_cost_ter_furn( dest_loc );
        ( move_cost <= 0 || move_cost > 2 ) && you.is_avatar() && !you.is_auto_moving() ) {
        // Generate context menu for bumped furniture.
        bool is_bump_zone = g->check_zone( zone_type_BUMP_INTERACT, dest_loc );
        tripoint_bub_ms dest_loc_bub = tripoint_bub_ms( dest_loc );
        bool can_examine = can_interact_at( ACTION_EXAMINE, dest_loc );
        bool can_pickup = can_interact_at( ACTION_PICKUP, dest_loc );
        int can_decon = 0;
        if( can_construct( construction_constr_deconstruct_simple.obj(), dest_loc_bub ) ) {
            can_decon = 3;
        } else if( can_construct( construction_constr_deconstruct.obj(), dest_loc_bub )
                   && player_can_build( you, you.crafting_inventory(),
                                        construction_constr_deconstruct.obj() ) ) {
            can_decon = 2;
        }
        bool enable_bumping = false;
        if( move_cost <= 0 ) {
            // Impassable.  Allow interaction if it's not a door.
            enable_bumping = !m.has_flag( "DOOR", dest_loc );
        } else {
            // Passable.  Enable if zone set, safe mode off and can examine or quick-deconstruct.
            bool has_prominent_interaction = can_examine || can_decon >= 3;
            enable_bumping = is_bump_zone
                             && g->safe_mode != SAFE_MODE_OFF && has_prominent_interaction;
        }
        if( enable_bumping ) {
            enum bump_actions {
                bump_move_onto = 1,
                bump_pickup,
                bump_examine,
                bump_deconstruct,
                //bump_smash,
            };
            uilist cmenu;
            cmenu.text = string_format( _( "What do you want to do with %s?" ), m.disp_name( dest_loc ) );
            if( move_cost > 0 ) {
                cmenu.addentry( bump_move_onto, true, 'm',
                                _( "Move onto %s.  (This will slow you down.)" ), m.disp_name( dest_loc ) );
            }
            if( can_pickup ) {
                cmenu.addentry( bump_pickup, true, 'g', _( "Access items in %s." ),
                                m.disp_name( dest_loc ) );
            }
            if( can_examine ) {
                // TODO ideally this text should be customized based on the examine action.
                cmenu.addentry( bump_examine, true, 'e', _( "Examine %s." ), m.disp_name( dest_loc ) );
            }
            if( can_decon > 0 ) {
                cmenu.addentry( bump_deconstruct, true, 'd', can_decon >= 3 ?
                                _( "Take down %s.  (This would take a few seconds.)" ) :
                                _( "Deconstruct %s.  (This would take some time.)" ),
                                m.disp_name( dest_loc ) );
            }
            // TODO: there's no subroutine for smashing things.
            /*if( m.is_bashable_ter_furn( dest_loc ) ) {
                cmenu.addentry( bump_smash, true, 's', _( "Smash %s." ), m.disp_name(dest_loc) );
            }*/

            // Note: following the example of doors, we return true when performing context actions.
            switch( cmenu.entries.size() ) {
                case 0: {
                    // Skip the menu if no interactions were possible.
                    cmenu.ret = bump_move_onto;
                    break;
                }
                case 1: {
                    // If the only option is move onto or pick up, automatically choose that.
                    switch( cmenu.entries[0].retval ) {
                        case bump_move_onto: {
                            cmenu.ret = bump_move_onto;
                            break;
                        }
                        case bump_pickup: {
                            cmenu.ret = bump_pickup;
                            break;
                        }
                        default: {
                            cmenu.query();
                            break;
                        }
                    }
                    break;
                }
                default: {
                    cmenu.query();
                    break;
                }
            }
            switch( cmenu.ret ) {
                default: {
                    // If the user canceled the menu, don't do anything.
                    return false;
                }
                case bump_move_onto: {
                    // Proceed to walk_move.
                    break;
                }
                case bump_pickup: {
                    you.pick_up( game_menus::inv::pickup( you, dest_loc ) );
                    return true;
                }
                case bump_examine: {
                    m.examine( you, dest_loc );
                    return true;
                }
                case bump_deconstruct: {
                    place_construction( { can_decon >= 3
                                          ? construction_constr_deconstruct_simple.obj().group
                                          : construction_constr_deconstruct.obj().group
                                        } );
                    return true;
                }
            }
        }
    }
    if( g->walk_move( dest_loc, via_ramp ) ) {
        return true;
    }
    if( g->phasing_move( dest_loc ) ) {
        return true;
    }
    if( veh_closed_door ) {
        if( !veh1->handle_potential_theft( you ) ) {
            return true;
        } else {
            door_name = veh1->part( dpart ).name();
            if( outside_vehicle ) {
                veh1->open_all_at( dpart );
            } else {
                veh1->open( dpart );
            }
            //~ %1$s - vehicle name, %2$s - part name
            you.add_msg_if_player( _( "You open the %1$s's %2$s." ), veh1->name, door_name );
        }
        you.moves -= 100;
        // if auto move is on, continue moving next turn
        if( you.is_auto_moving() ) {
            you.defer_move( dest_loc );
        }
        return true;
    }

    if( m.furn( dest_loc ) != f_safe_c && m.open_door( you, dest_loc, !m.is_outside( you.pos() ) ) ) {
        you.moves -= 100;
        if( veh1 != nullptr ) {
            //~ %1$s - vehicle name, %2$s - part name
            you.add_msg_if_player( _( "You open the %1$s's %2$s." ), veh1->name, door_name );
        } else {
            you.add_msg_if_player( _( "You open the %s." ), door_name );
        }
        // if auto move is on, continue moving next turn
        if( you.is_auto_moving() ) {
            you.defer_move( dest_loc );
        }
        return true;
    }

    // Invalid move
    const bool waste_moves = you.is_blind() || you.has_effect( effect_stunned );
    if( waste_moves || dest_loc.z != you.posz() ) {
        add_msg( _( "You bump into the %s!" ), m.obstacle_name( dest_loc ) );
        // Only lose movement if we're blind
        if( waste_moves ) {
            you.moves -= 100;
        }
    } else if( m.ter( dest_loc ) == t_door_locked || m.ter( dest_loc ) == t_door_locked_peep ||
               m.ter( dest_loc ) == t_door_locked_alarm || m.ter( dest_loc ) == t_door_locked_interior ) {
        // Don't drain move points for learning something you could learn just by looking
        add_msg( _( "That door is locked!" ) );
    } else if( m.ter( dest_loc ) == t_door_bar_locked ) {
        add_msg( _( "You rattle the bars but the door is locked!" ) );
    }
    return false;
}

bool avatar_action::ramp_move( avatar &you, map &m, const tripoint &dest_loc )
{
    if( dest_loc.z != you.posz() ) {
        // No recursive ramp_moves
        return false;
    }

    // We're moving onto a tile with no support, check if it has a ramp below
    if( !m.has_floor_or_support( dest_loc ) ) {
        tripoint below( dest_loc.xy(), dest_loc.z - 1 );
        if( m.has_flag( ter_furn_flag::TFLAG_RAMP, below ) ) {
            // But we're moving onto one from above
            const tripoint dp = dest_loc - you.pos();
            move( you, m, tripoint( dp.xy(), -1 ) );
            // No penalty for misaligned stairs here
            // Also cheaper than climbing up
            return true;
        }

        return false;
    }

    if( !m.has_flag( ter_furn_flag::TFLAG_RAMP, you.pos() ) ||
        m.passable( dest_loc ) ) {
        return false;
    }

    // Try to find an aligned end of the ramp that will make our climb faster
    // Basically, finish walking on the stairs instead of pulling self up by hand
    bool aligned_ramps = false;
    for( const tripoint &pt : m.points_in_radius( you.pos(), 1 ) ) {
        if( rl_dist( pt, dest_loc ) < 2 && m.has_flag( ter_furn_flag::TFLAG_RAMP_END, pt ) ) {
            aligned_ramps = true;
            break;
        }
    }

    const tripoint above_u( you.posx(), you.posy(), you.posz() + 1 );
    if( m.has_floor_or_support( above_u ) ) {
        add_msg( m_warning, _( "You can't climb here - there's a ceiling above." ) );
        return false;
    }

    const tripoint dp = dest_loc - you.pos();
    const tripoint old_pos = you.pos();
    move( you, m, tripoint( dp.xy(), 1 ) );
    // We can't just take the result of the above function here
    if( you.pos() != old_pos ) {
        you.moves -= 50 + ( aligned_ramps ? 0 : 50 );
    }

    return true;
}

void avatar_action::swim( map &m, avatar &you, const tripoint &p )
{
    if( !m.has_flag( ter_furn_flag::TFLAG_SWIMMABLE, p ) ) {
        dbg( D_ERROR ) << "game:plswim: Tried to swim in "
                       << m.tername( p ) << "!";
        debugmsg( "Tried to swim in %s!", m.tername( p ) );
        return;
    }
    if( you.has_effect( effect_onfire ) ) {
        add_msg( _( "The water puts out the flames!" ) );
        you.remove_effect( effect_onfire );
        if( you.is_mounted() ) {
            monster *mon = you.mounted_creature.get();
            if( mon->has_effect( effect_onfire ) ) {
                mon->remove_effect( effect_onfire );
            }
        }
    }
    if( you.has_effect( effect_glowing ) ) {
        add_msg( _( "The water washes off the glowing goo!" ) );
        you.remove_effect( effect_glowing );
    }

    g->water_affect_items( you );

    int movecost = you.swim_speed();
    you.practice( skill_swimming, you.is_underwater() ? 2 : 1 );
    if( movecost >= 500 || you.has_effect( effect_winded ) ) {
        if( !you.is_underwater() &&
            !( you.shoe_type_count( itype_swim_fins ) == 2 ||
               ( you.shoe_type_count( itype_swim_fins ) == 1 && one_in( 2 ) ) ) ) {
            add_msg( m_bad, _( "You sink like a rock!" ) );
            you.set_underwater( true );
        }
    }
    if( you.oxygen <= 5 && you.is_underwater() ) {
        if( movecost < 500 ) {
            popup( _( "You need to breathe!  (%s to surface.)" ), press_x( ACTION_MOVE_UP ) );
        } else {
            popup( _( "You need to breathe but you can't swim!  Get to dry land, quick!" ) );
        }
    }
    bool diagonal = p.x != you.posx() && p.y != you.posy();
    if( you.in_vehicle ) {
        m.unboard_vehicle( you.pos() );
    }
    if( you.is_mounted() && m.veh_at( you.pos() ).part_with_feature( VPFLAG_BOARDABLE, true ) ) {
        add_msg( m_warning, _( "You cannot board a vehicle while mounted." ) );
        return;
    }
    if( const auto vp = m.veh_at( p ).part_with_feature( VPFLAG_BOARDABLE, true ) ) {
        if( !vp->vehicle().handle_potential_theft( you ) ) {
            return;
        }
    }
    tripoint old_abs_pos = m.getabs( you.pos() );
    you.setpos( p );
    g->update_map( you );

    cata_event_dispatch::avatar_moves( old_abs_pos, you, m );

    if( m.veh_at( you.pos() ).part_with_feature( VPFLAG_BOARDABLE, true ) ) {
        m.board_vehicle( you.pos(), &you );
    }
    you.moves -= ( movecost > 200 ? 200 : movecost ) * ( trigdist && diagonal ? M_SQRT2 : 1 );
    you.inv->rust_iron_items();

    if( !you.is_mounted() ) {
        you.burn_move_stamina( movecost );
    }

    body_part_set flags;
    if( !you.is_underwater() ) {
        flags = you.get_drenching_body_parts( false, true, true );
    } else {
        flags = you.get_drenching_body_parts();
    }

    you.drench( 100, flags, false );
}

static float rate_critter( const Creature &c )
{
    const npc *np = dynamic_cast<const npc *>( &c );
    if( np != nullptr ) {
        item_location wielded = np->get_wielded_item();
        if( wielded ) {
            return np->weapon_value( *wielded );
        } else {
            return np->unarmed_value();
        }
    }

    const monster *m = dynamic_cast<const monster *>( &c );
    return m->type->difficulty;
}

void avatar_action::autoattack( avatar &you, map &m )
{
    const item_location weapon = you.get_wielded_item();
    int reach = weapon ? weapon->reach_range( you ) : 1;
    std::vector<Creature *> critters = you.get_targetable_creatures( reach, true );
    critters.erase( std::remove_if( critters.begin(), critters.end(), [&you,
    reach]( const Creature * c ) {
        if( reach == 1 && !you.is_adjacent( c, true ) ) {
            return true;
        }
        if( !c->is_npc() ) {
            return false;
        }
        return !dynamic_cast<const npc &>( *c ).is_enemy();
    } ), critters.end() );
    if( critters.empty() ) {
        add_msg( m_info, _( "No hostile creature in reach.  Waiting a turn." ) );
        if( g->check_safe_mode_allowed() ) {
            you.pause();
        }
        return;
    }

    Creature &best = **std::max_element( critters.begin(), critters.end(),
    []( const Creature * l, const Creature * r ) {
        return rate_critter( *l ) > rate_critter( *r );
    } );

    const tripoint diff = best.pos() - you.pos();
    if( std::abs( diff.x ) <= 1 && std::abs( diff.y ) <= 1 && diff.z == 0 ) {
        move( you, m, tripoint( diff.xy(), 0 ) );
        return;
    }

    you.reach_attack( best.pos() );
}

// TODO: Move data/functions related to targeting out of game class
bool avatar_action::can_fire_weapon( avatar &you, const map &m, const item &weapon )
{
    if( !weapon.is_gun() ) {
        debugmsg( "Expected item to be a gun" );
        return false;
    }

    if( !you.try_break_relax_gas( _( "Your eyes steel, and you raise your weapon!" ),
                                  _( "You can't fire your weapon, it's too heavy…" ) ) ) {
        return false;
    }

    std::vector<std::string> messages;

    for( const std::pair<const gun_mode_id, gun_mode> &mode_map : weapon.gun_all_modes() ) {
        bool check_common = gunmode_checks_common( you, m, messages, mode_map.second );
        bool check_weapon = gunmode_checks_weapon( you, m, messages, mode_map.second );
        bool can_use_mode = check_common && check_weapon;
        if( can_use_mode ) {
            return true;
        }
    }

    for( const std::string &message : messages ) {
        add_msg( m_info, message );
    }
    return false;
}

void avatar_action::fire_wielded_weapon( avatar &you )
{
    const item_location weapon = you.get_wielded_item();

    if( !weapon ) {
        return;
    }

    if( weapon->is_gunmod() ) {
        add_msg( m_info,
                 _( "The %s must be attached to a gun, it can not be fired separately." ),
                 weapon->tname() );
        return;
    } else if( !weapon->is_gun() ) {
        return;
    } else if( weapon->ammo_data() &&
               !weapon->ammo_types().count( weapon->loaded_ammo().ammo_type() ) ) {
        add_msg( m_info, _( "The %s can't be fired while loaded with incompatible ammunition %s" ),
                 weapon->tname(), weapon->ammo_current()->nname( 1 ) );
        return;
    }

    you.assign_activity( aim_activity_actor::use_wielded() );
}

void avatar_action::fire_ranged_mutation( Character &you, const item &fake_gun )
{
    you.assign_activity( aim_activity_actor::use_mutation( fake_gun ) );
}

void avatar_action::fire_ranged_bionic( avatar &you, const item &fake_gun )
{
    you.assign_activity( aim_activity_actor::use_bionic( fake_gun ) );
}

bool avatar_action::fire_turret_manual( avatar &you, map &m, turret_data &turret )
{
    if( !turret.base()->is_gun() ) {
        debugmsg( "Expected turret base to be a gun." );
        return false;
    }

    switch( turret.query() ) {
        case turret_data::status::no_ammo:
            add_msg( m_bad, _( "The %s is out of ammo." ), turret.name() );
            return false;
        case turret_data::status::no_power:
            add_msg( m_bad, _( "The %s is not powered." ), turret.name() );
            return false;
        case turret_data::status::ready:
            break;
        default:
            debugmsg( "Unknown turret status" );
            return false;
    }

    // check if any gun modes are usable
    std::vector<std::string> messages;
    const std::map<gun_mode_id, gun_mode> gunmodes = turret.base()->gun_all_modes();
    if( !std::any_of( gunmodes.begin(), gunmodes.end(),
    [&you, &m, &messages]( const std::pair<const gun_mode_id, gun_mode> &p ) {
    return gunmode_checks_common( you, m, messages, p.second );
    } ) ) {
        // no gunmode is usable, dump reason messages why not
        for( const std::string &msg : messages ) {
            add_msg( m_bad, msg );
        }
        return false;
    }

    // all checks passed - start aiming
    g->temp_exit_fullscreen();
    target_handler::trajectory trajectory = target_handler::mode_turret_manual( you, turret );

    if( !trajectory.empty() ) {
        turret.fire( you, trajectory.back() );
    }
    g->reenter_fullscreen();
    return true;
}

void avatar_action::mend( avatar &you, item_location loc )
{

    if( you.fine_detail_vision_mod() > 4 ) {
        add_msg( m_bad, _( "It's too dark to work on mending this." ) );
        return;
    }

    if( !loc ) {
        if( you.is_armed() ) {
            loc = you.get_wielded_item();
        } else {
            add_msg( m_info, _( "You're not wielding anything." ) );
            return;
        }
    }

    if( you.has_item( *loc ) ) {
        you.mend_item( item_location( loc ) );
    }
}

bool avatar_action::eat_here( avatar &you )
{
    map &here = get_map();
    if( ( you.has_active_mutation( trait_RUMINANT ) || you.has_active_mutation( trait_GRAZER ) ) &&
        ( here.ter( you.pos() ) == t_underbrush || here.ter( you.pos() ) == t_shrub ) ) {
        if( you.has_effect( effect_hunger_engorged ) ) {
            add_msg( _( "You're too full to eat the leaves from the %s." ), here.ter( you.pos() )->name() );
            return true;
        } else {
            here.ter_set( you.pos(), t_grass );
            item food( "underbrush", calendar::turn, 1 );
            you.assign_activity( consume_activity_actor( food ) );
            return true;
        }
    }
    if( you.has_active_mutation( trait_GRAZER ) && ( here.ter( you.pos() ) == t_grass ||
            here.ter( you.pos() ) == t_grass_long || here.ter( you.pos() ) == t_grass_tall ) ) {
        if( you.has_effect( effect_hunger_engorged ) ) {
            add_msg( _( "You're too full to graze." ) );
            return true;
        } else {
            item food( item( "grass", calendar::turn, 1 ) );
            you.assign_activity( consume_activity_actor( food ) );
            if( here.ter( you.pos() ) == t_grass_tall ) {
                here.ter_set( you.pos(), t_grass_long );
            } else if( here.ter( you.pos() ) == t_grass_long ) {
                here.ter_set( you.pos(), t_grass );
            } else {
                here.ter_set( you.pos(), t_dirt );
            }
            return true;
        }
    }
    if( you.has_active_mutation( trait_GRAZER ) ) {
        if( here.ter( you.pos() ) == t_grass_golf ) {
            add_msg( _( "This grass is too short to graze." ) );
            return true;
        } else if( here.ter( you.pos() ) == t_grass_dead ) {
            add_msg( _( "This grass is dead and too mangled for you to graze." ) );
            return true;
        } else if( here.ter( you.pos() ) == t_grass_white ) {
            add_msg( _( "This grass is tainted with paint and thus inedible." ) );
            return true;
        }
    }
    return false;
}

void avatar_action::eat( avatar &you, item_location &loc )
{
    std::string filter;
    if( !you.activity.str_values.empty() ) {
        filter = you.activity.str_values.back();
    }
    avatar_action::eat( you, loc, you.activity.values, you.activity.targets, filter,
                        you.activity.id() );
}

void avatar_action::eat( avatar &you, item_location &loc,
                         const std::vector<int> &consume_menu_selections,
                         const std::vector<item_location> &consume_menu_selected_items,
                         const std::string &consume_menu_filter,
                         activity_id type )
{
    if( !loc ) {
        you.cancel_activity();
        add_msg( _( "Never mind." ) );
        return;
    }
    loc.overflow();
    you.assign_activity( consume_activity_actor( loc, consume_menu_selections,
                         consume_menu_selected_items, consume_menu_filter, type ) );
    you.last_item = item( *loc ).typeId();
}

void avatar_action::eat_or_use( avatar &you, item_location loc )
{
    if( loc && loc->is_medical_tool() ) {
        avatar_action::use_item( you, loc, "heal" );
    } else {
        avatar_action::eat( you, loc );
    }
}

void avatar_action::plthrow( avatar &you, item_location loc,
                             const std::optional<tripoint> &blind_throw_from_pos )
{
    bool in_shell = you.has_active_mutation( trait_SHELL2 ) ||
                    you.has_active_mutation( trait_SHELL3 );
    if( in_shell ) {
        add_msg( m_info, _( "You can't effectively throw while you're in your shell." ) );
        return;
    } else if( you.has_effect( effect_incorporeal ) ) {
        add_msg( m_info, _( "You lack the substance to affect anything." ) );
        return;
    }
    if( you.is_mounted() ) {
        monster *mons = get_player_character().mounted_creature.get();
        if( mons->has_flag( mon_flag_RIDEABLE_MECH ) ) {
            if( !mons->check_mech_powered() ) {
                add_msg( m_bad, _( "Your %s refuses to move as its batteries have been drained." ),
                         mons->get_name() );
                return;
            }
        }
    }

    if( !loc ) {
        loc = game_menus::inv::titled_menu( you,  _( "Throw item" ),
                                            _( "You don't have any items to throw." ) );
    }

    if( !loc ) {
        add_msg( _( "Never mind." ) );
        return;
    }

    const ret_val<void> ret = you.can_wield( *loc );
    if( !ret.success() ) {
        add_msg( m_info, "%s", ret.c_str() );
        return;
    }

    // make a copy and get the original.
    // the copy is thrown and has its and the originals charges set appropriately
    // or deleted from inventory if its charges(1) or not stackable.
    item *orig = loc.get_item();
    item thrown = *orig;
    int range = you.throw_range( thrown );
    if( range < 0 ) {
        add_msg( m_info, _( "You don't have that item." ) );
        return;
    } else if( range == 0 ) {
        add_msg( m_info, _( "That is too heavy to throw." ) );
        return;
    }

    if( you.is_wielding( *orig ) && orig->has_flag( flag_NO_UNWIELD ) ) {
        // pos == -1 is the weapon, NO_UNWIELD is used for bio_claws_weapon
        add_msg( m_info, _( "That's part of your body, you can't throw that!" ) );
        return;
    }

    if( !you.try_break_relax_gas( _( "You concentrate mightily, and your body obeys!" ),
                                  _( "You can't muster up the effort to throw anything…" ) ) ) {
        return;
    }
    // if you're wearing the item you need to be able to take it off
    if( you.is_worn( *orig ) ) {
        ret_val<void> ret = you.can_takeoff( *orig );
        if( !ret.success() ) {
            add_msg( m_info, "%s", ret.c_str() );
            return;
        }
    }
    // you must wield the item to throw it
    if( !you.is_wielding( *orig ) ) {
        if( !you.wield( *orig ) ) {
            return;
        }
    }

    // Shift our position to our "peeking" position, so that the UI
    // for picking a throw point lets us target the location we couldn't
    // otherwise see.
    const tripoint original_player_position = you.pos();
    if( blind_throw_from_pos ) {
        you.setpos( *blind_throw_from_pos );
    }

    g->temp_exit_fullscreen();

    item_location weapon = you.get_wielded_item();
    target_handler::trajectory trajectory = target_handler::mode_throw( you, *weapon,
                                            blind_throw_from_pos.has_value() );

    // If we previously shifted our position, put ourselves back now that we've picked our target.
    if( blind_throw_from_pos ) {
        you.setpos( original_player_position );
    }

    if( trajectory.empty() ) {
        return;
    }

    if( weapon->count_by_charges() && weapon->charges > 1 ) {
        weapon->mod_charges( -1 );
        thrown.charges = 1;
    } else {
        you.remove_weapon();
    }
    you.throw_item( trajectory.back(), thrown, blind_throw_from_pos );
    g->reenter_fullscreen();
}

static void update_lum( item_location loc, bool add )
{
    switch( loc.where() ) {
        case item_location::type::map:
            get_map().update_lum( loc, add );
            break;
        default:
            break;
    }
}

void avatar_action::use_item( avatar &you )
{
    item_location loc;
    avatar_action::use_item( you, loc );
}

void avatar_action::use_item( avatar &you, item_location &loc, std::string const &method )
{
    if( you.has_effect( effect_incorporeal ) ) {
        you.add_msg_if_player( m_bad, _( "You can't use anything while incorporeal." ) );
        return;
    }

    // Some items may be used without being picked up first
    bool use_in_place = false;

    if( !loc ) {
        loc = game_menus::inv::use( you );

        if( !loc ) {
            add_msg( _( "Never mind." ) );
            return;
        }
    }

    loc.overflow();

    if( loc->is_comestible() && loc->is_frozen_liquid() ) {
        add_msg( _( "Try as you might, you can't consume frozen liquids." ) );
        return;
    }

    if( loc->wetness && loc->has_flag( flag_WATER_BREAK_ACTIVE ) ) {
        if( query_yn( _( "This item is still wet and it will break if you turn it on. Proceed?" ) ) ) {
            loc->deactivate();
            loc->set_flag( flag_ITEM_BROKEN );
        } else {
            return;
        }
    }

    item_pocket *parent_pocket = nullptr;
    bool on_person = true;
    int pre_obtain_moves = you.moves;
    if( loc->has_flag( flag_ALLOWS_REMOTE_USE ) || you.is_worn( *loc ) ) {
        use_in_place = true;
        // Activate holster on map only if hands are free.
    } else if( you.can_wield( *loc ).success() && loc->is_holster() && !loc.held_by( you ) ) {
        use_in_place = true;
        // Adjustment because in player::wield_contents this amount is refunded.
        you.mod_moves( -loc.obtain_cost( you ) );
    } else {
        item_location::type loc_where = loc.where_recursive();
        std::string const name = loc->display_name();
        if( loc_where != item_location::type::character ) {
            pre_obtain_moves = -1;
            on_person = false;
        }

        // Get the parent pocket before the item is obtained.
        if( loc.has_parent() ) {
            parent_pocket = loc.parent_pocket();
        }

        loc = loc.obtain( you, 1 );

        if( parent_pocket ) {
            parent_pocket->on_contents_changed();
        }
        if( pre_obtain_moves == -1 ) {
            pre_obtain_moves = you.moves;
        }
        if( !loc ) {
            you.add_msg_if_player( _( "Couldn't pick up the %s." ), name );
            return;
        }
        if( loc_where != item_location::type::character ) {
            you.add_msg_if_player( _( "You pick up the %s." ), name );
        }
    }

    if( use_in_place ) {
        update_lum( loc, false );
        you.use( loc, pre_obtain_moves, method );
        if( loc ) {
            update_lum( loc, true );
            loc.make_active();
        }
    } else {
        you.use( loc, pre_obtain_moves, method );

        if( parent_pocket && on_person && parent_pocket->will_spill() ) {
            parent_pocket->handle_liquid_or_spill( you, loc.parent_item().get_item() );
        }
    }
    if( loc ) {
        loc.on_contents_changed();
    }

    you.recoil = MAX_RECOIL;

    you.invalidate_crafting_inventory();
}

// Opens up a menu to Unload a container, gun, or tool
// If it's a gun, some gunmods can also be loaded
void avatar_action::unload( avatar &you )
{
    item_location loc = g->inv_map_splice( [&you]( const item & it ) {
        return you.rate_action_unload( it ) == hint_rating::good;
    }, _( "Unload item" ), 1, _( "You have nothing to unload." ) );

    if( !loc ) {
        add_msg( _( "Never mind." ) );
        return;
    }

    you.unload( loc );
}
