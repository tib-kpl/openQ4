// Copyright (C) 2026 DarkMatter Productions. GPL-3.0-or-later.
//
// openQ4 gamepad aim assist for single player, included into Player.cpp.
//
// Quake 4's own aim assistance only exists in its console builds (_XBOX and
// _XENON); the PC game has none. This restores the same ideas, built on the
// player's view angles instead of the console input layer, and it only acts
// while a controller was the last device used, so a mouse is never touched:
//
//   friction    the view turns more slowly while the crosshair is near a target
//   stickiness  a gentle pull toward the target while the stick is held
//   tracking    a share of the target's own angular motion follows it, so a
//               strafing enemy (or the player's own strafing) stays on the
//               crosshair
//   bullet      a shot fired close to a target is bent onto it
//
// g_aimAssist picks the strength: 0 off, 1 low, 2 medium, 3 high.

struct openQ4AimAssistTuning_t {
	float	cone;			// degrees around the crosshair that a target can influence the view
	float	inner;			// degrees inside which the assist is at full strength
	float	slowMin;		// share of the stick's turn kept at full strength (friction)
	float	pullRate;		// stickiness, in shares of the remaining error closed per second
	float	tracking;		// share of the target's angular motion that is followed
	float	bullet;			// degrees within which a shot is bent onto a target
	float	range;			// units: nothing farther is assisted
};

static const openQ4AimAssistTuning_t openQ4AimAssistLevels[3] = {
	{  5.0f, 2.0f, 0.80f, 2.0f, 0.25f, 1.0f, 1500.0f },	// low
	{  7.0f, 3.0f, 0.65f, 4.0f, 0.50f, 2.0f, 1800.0f },	// medium
	{ 10.0f, 4.5f, 0.50f, 7.0f, 0.75f, 3.5f, 2200.0f },	// high
};

/*
==============
idPlayer::AimAssistTuning

The tuning for g_aimAssist, or NULL when the assist is off or not available.
==============
*/
const void *idPlayer::AimAssistTuning( void ) const {
	const int level = g_aimAssist.GetInteger();
	if ( level <= 0 ) {
		return NULL;
	}
	// A mouse, and every other player, is left exactly as it was: only the one
	// local player of a single-player game, and only on a controller.
	if ( gameLocal.isMultiplayer || gameLocal.GetLocalPlayer() != this ) {
		return NULL;
	}
	if ( !cvarSystem->GetCVarBool( "ui_radialStickActive" ) ) {
		return NULL;
	}
	if ( health <= 0 || noclip || influenceActive || gameLocal.inCinematic || privateCameraView || gameLocal.GetCamera() ) {
		return NULL;
	}
	if ( weaponWheelActive || ( vrSystem != NULL && vrSystem->IsActive() ) ) {
		return NULL;
	}
	return &openQ4AimAssistLevels[ idMath::ClampInt( 1, 3, level ) - 1 ];
}

/*
==============
idPlayer::AimAssistFindTarget

The hostile that sits closest to the crosshair, within `coneDeg` (widened for a
target that looks bigger than the cone) and `range`, and that can be seen.
Returns the point to aim at and the angles from `eye` to it.
==============
*/
idEntity *idPlayer::AimAssistFindTarget( const idVec3 &eye, const idAngles &view, float coneDeg, float range,
										 idVec3 &point, idAngles &targetAngles, float &effectiveCone ) {
	idEntity	*best = NULL;
	float		bestScore = 1.0f;

	for ( idEntity *ent = gameLocal.activeEntities.Next(); ent != NULL; ent = ent->activeNode.Next() ) {
		if ( !ent->IsType( idAI::GetClassType() ) ) {
			continue;
		}
		idAI *ai = static_cast<idAI *>( ent );
		if ( ai->health <= 0 || ai->IsHidden() || !ai->fl.takedamage || ai->team == team ) {
			continue;
		}

		const idBounds &bounds = ai->GetPhysics()->GetAbsBounds();
		idVec3 aim = bounds.GetCenter();
		aim.z = bounds[ 0 ].z + ( bounds[ 1 ].z - bounds[ 0 ].z ) * 0.65f;	// the chest

		idVec3 dir = aim - eye;
		const float dist = dir.Normalize();
		if ( dist > range || dist < 24.0f ) {
			continue;
		}

		// A target looks bigger up close: never ask for less than its own size.
		const float halfWidth = 0.5f * Min( bounds[1].x - bounds[0].x, bounds[1].y - bounds[0].y );
		const float apparent = RAD2DEG( idMath::ATan( halfWidth / dist ) );
		const float cone = Max( coneDeg, apparent * 1.25f );

		const idAngles angles = dir.ToAngles();
		const float errYaw = idMath::AngleDelta( angles.yaw, view.yaw );
		const float errPitch = idMath::AngleDelta( angles.pitch, view.pitch );
		const float score = idMath::Sqrt( errYaw * errYaw + errPitch * errPitch ) / cone;
		if ( score >= bestScore ) {
			continue;
		}

		// Only what is in the open: a wall between is not a target.
		trace_t trace;
		gameLocal.TracePoint( this, trace, eye, aim, MASK_SHOT_BOUNDINGBOX, this );
		if ( trace.fraction < 1.0f && trace.c.entityNum != ai->entityNumber ) {
			idEntity *hit = gameLocal.entities[ trace.c.entityNum ];
			const bool ownBody = hit != NULL && hit->IsType( idAFAttachment::GetClassType() ) &&
				static_cast<idAFAttachment *>( hit )->GetBody() == ai;
			if ( !ownBody ) {
				continue;
			}
		}

		best = ai;
		bestScore = score;
		point = aim;
		targetAngles = angles;
		effectiveCone = cone;
	}
	return best;
}

/*
==============
idPlayer::AimAssistUpdateView

Called by UpdateViewAngles once this frame's stick input has been added to the
view. `previousView` is where the view was before that input.
==============
*/
void idPlayer::AimAssistUpdateView( const idAngles &previousView ) {
	const openQ4AimAssistTuning_t *tuning = static_cast<const openQ4AimAssistTuning_t *>( AimAssistTuning() );
	if ( tuning == NULL || weapon == NULL ) {
		aimAssistTarget = NULL;
		aimAssistLastTime = 0;
		return;
	}

	const float dt = aimAssistLastTime > 0 ? idMath::ClampFloat( 0.001f, 0.1f, MS2SEC( gameLocal.time - aimAssistLastTime ) ) : 0.016f;
	aimAssistLastTime = gameLocal.time;

	const float dYaw = idMath::AngleDelta( viewAngles.yaw, previousView.yaw );
	const float dPitch = idMath::AngleDelta( viewAngles.pitch, previousView.pitch );
	const float input = idMath::Sqrt( dYaw * dYaw + dPitch * dPitch );

	idVec3 point;
	idAngles targetAngles;
	float cone = tuning->cone;
	idEntity *target = AimAssistFindTarget( GetEyePosition(), viewAngles, tuning->cone, tuning->range, point, targetAngles, cone );
	if ( target == NULL ) {
		aimAssistTarget = NULL;
		return;
	}

	// 1 at the inner cone, falling to 0 at the edge of the outer one.
	const float inner = cone * ( tuning->inner / tuning->cone );
	float errYaw = idMath::AngleDelta( targetAngles.yaw, viewAngles.yaw );
	float errPitch = idMath::AngleDelta( targetAngles.pitch, viewAngles.pitch );
	const float err = idMath::Sqrt( errYaw * errYaw + errPitch * errPitch );
	const float weight = 1.0f - idMath::ClampFloat( 0.0f, 1.0f, ( err - inner ) / Max( cone - inner, 0.01f ) );

	// friction: the stick turns the view less while a target is near
	if ( input > 0.0f ) {
		const float keep = 1.0f - ( 1.0f - tuning->slowMin ) * weight;
		viewAngles.yaw = idMath::AngleNormalize180( previousView.yaw + dYaw * keep );
		viewAngles.pitch = idMath::AngleNormalize180( previousView.pitch + dPitch * keep );
		errYaw = idMath::AngleDelta( targetAngles.yaw, viewAngles.yaw );
		errPitch = idMath::AngleDelta( targetAngles.pitch, viewAngles.pitch );
	}

	// stickiness: only while the stick is held, so a still stick never moves the view
	if ( input > 0.05f ) {
		const float pull = Min( 1.0f, tuning->pullRate * dt * weight );
		viewAngles.yaw = idMath::AngleNormalize180( viewAngles.yaw + errYaw * pull );
		viewAngles.pitch = idMath::AngleNormalize180( viewAngles.pitch + errPitch * pull );
	}

	// tracking: follow the target's own angular motion, for the same target as last frame
	if ( aimAssistTarget.GetEntity() == target && ( input > 0.05f || ( usercmd.buttons & BUTTON_ATTACK ) ) ) {
		const float moveYaw = idMath::AngleDelta( targetAngles.yaw, aimAssistLastAngles.yaw );
		const float movePitch = idMath::AngleDelta( targetAngles.pitch, aimAssistLastAngles.pitch );
		// a jump to another place (a teleport, a respawn) is not motion to follow
		if ( idMath::Fabs( moveYaw ) < 8.0f && idMath::Fabs( movePitch ) < 8.0f ) {
			const float follow = tuning->tracking * weight;
			viewAngles.yaw = idMath::AngleNormalize180( viewAngles.yaw + moveYaw * follow );
			viewAngles.pitch = idMath::AngleNormalize180( viewAngles.pitch + movePitch * follow );
		}
	}

	aimAssistTarget = target;
	aimAssistLastAngles = targetAngles;
}

/*
==============
idPlayer::AimAssistRedirect

Bends a shot that would pass close to a target onto it. Called by the weapon
with the muzzle it is about to fire from; returns whether the aim moved.
==============
*/
bool idPlayer::AimAssistRedirect( const idVec3 &origin, idMat3 &axis ) {
	const openQ4AimAssistTuning_t *tuning = static_cast<const openQ4AimAssistTuning_t *>( AimAssistTuning() );
	if ( tuning == NULL || tuning->bullet <= 0.0f ) {
		return false;
	}

	idVec3 point;
	idAngles targetAngles;
	float cone = tuning->bullet;
	idEntity *target = AimAssistFindTarget( origin, axis[ 0 ].ToAngles(), tuning->bullet, tuning->range, point, targetAngles, cone );
	if ( target == NULL ) {
		return false;
	}

	idVec3 want = point - origin;
	want.Normalize();
	const idVec3 have = axis[ 0 ];
	const float cosine = idMath::ClampFloat( -1.0f, 1.0f, have * want );
	const float angle = idMath::ACos( cosine );
	if ( angle < 0.0001f ) {
		return false;
	}
	idVec3 pivot = have.Cross( want );
	if ( pivot.Normalize() < 0.0001f ) {
		return false;
	}

	// Rodrigues: turn every axis by the angle between the old and wanted forward
	const float c = idMath::Cos( angle ), s = idMath::Sin( angle );
	for ( int i = 0; i < 3; i++ ) {
		const idVec3 v = axis[ i ];
		axis[ i ] = v * c + pivot.Cross( v ) * s + pivot * ( ( pivot * v ) * ( 1.0f - c ) );
	}
	return true;
}
