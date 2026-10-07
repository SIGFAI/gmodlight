-- GModLight: Garry's Mod runs hidden next to Dying Light and supplies the spawn
-- menu, physgun and physics. Only does anything when Dying Light's GModLight
-- plugin is running; normal Garry's Mod play is untouched.
AddCSLuaFile()

if not game.SinglePlayer() then return end
if not util.IsBinaryModuleInstalled or not util.IsBinaryModuleInstalled("gmodlight") then return end
if not pcall(require, "gmodlight") or not gmodlight then return end

CreateConVar("gmodlight_debug", "0", FCVAR_REPLICATED, "Draw GModLight actor boxes")
local cvFlipX = CreateConVar("gmodlight_flipx", "0", FCVAR_REPLICATED, "Mirror the Dying Light X axis if the views disagree")
local cvFovHorizontal = CreateConVar("gmodlight_fov_horizontal", "0", FCVAR_REPLICATED, "Dying Light reports horizontal FOV")
local cvScaleRenderFov = CLIENT and CreateClientConVar("gmodlight_scale_render_fov", "1", true, false, "Widen RenderView's fov to the screen's aspect")
-- 1.15: a bit more of GMod's arms and sleeves in view (and more margin before the
-- cut-off end of the arm model can show at the bottom while the gun moves).
local cvViewmodelFov = CLIENT and CreateClientConVar("gmodlight_viewmodel_scale", "1.15", true, false, "Multiplies the weapon's viewmodel fov (bigger = smaller gun, more arms)")

GML = GML or {}
local M = 52.4934  -- Source units per meter
local MODE_PLAY, MODE_MENU, MODE_GMOD, MODE_CONTEXT = 0, 1, 2, 3  -- shared/bridge.h Mode

-- ---------- Dying Light <-> Source space ----------
-- Dying Light: meters, Y up. Source: units, Z up. Everything is relative to an
-- anchor so we stay well inside Source's +-16384 unit world.

-- Swapping Y and Z maps a left-handed Y-up world onto Source's right-handed Z-up
-- one. If Dying Light turns out right-handed, X has to be mirrored too. Its
-- camera's left vector settles it: in a left-handed world, left = forward x up.
GML.mirror = GML.mirror or false

local function flipX()
	return GML.mirror ~= cvFlipX:GetBool()
end

local function anchor()
	return GetGlobal2Vector("gml_anchor_dl", vector_origin), GetGlobal2Vector("gml_origin_src", vector_origin)
end

function GML.ToSource(x, y, z)
	local a, o = anchor()
	local sx = (x - a.x) * M
	if flipX() then sx = -sx end
	return o + Vector(sx, (z - a.z) * M, (y - a.y) * M)
end

function GML.ToDL(v)
	local a, o = anchor()
	local d = v - o
	local dx = d.x / M
	if flipX() then dx = -dx end
	return a.x + dx, a.y + d.z / M, a.z + d.y / M
end

local function dirToSource(x, y, z)
	if flipX() then x = -x end
	return Vector(x, z, y)
end

-- Camera in Source space: eye, angles, Source-style (4:3 horizontal) fov.
function GML.Camera()
	local ok, px, py, pz, fx, fy, fz, ux, uy, uz, fov, aspect, _, _, lx, ly, lz, ctime, latency = gmodlight.GetCamera()
	if not ok then return nil end
	if lx and (lx * lx + ly * ly + lz * lz) > 0.25 then
		-- forward x up, compared with DL's own left
		local cx, cy, cz = fy * uz - fz * uy, fz * ux - fx * uz, fx * uy - fy * ux
		local mirror = (cx * lx + cy * ly + cz * lz) < 0
		if mirror ~= GML.mirror or not GML.handednessLogged then
			GML.mirror = mirror
			GML.handednessLogged = true
			gmodlight.Log("Dying Light is " .. (mirror and "right" or "left") .. "-handed; X " .. (mirror and "mirrored" or "as is"))
		end
	end
	local eye = GML.ToSource(px, py, pz)
	local ang = dirToSource(fx, fy, fz):AngleEx(dirToSource(ux, uy, uz))
	local vfov
	if cvFovHorizontal:GetBool() then
		vfov = 2 * math.atan(math.tan(math.rad(fov) / 2) / math.max(aspect, 0.1))
	else
		vfov = math.rad(fov)
	end
	local srcFov = math.deg(2 * math.atan(math.tan(vfov / 2) * 4 / 3))
	return eye, ang, srcFov, Vector(px, py, pz), ctime, latency
end

-- The camera to draw with. Dying Light shows GMod's frames 10-25 ms after the
-- camera sample they were drawn from; turning is re-projected in Dying Light, but
-- moving isn't, so near things (a grenade at your feet) slid while walking. Drawn
-- from where the camera will be by then instead (velocity from DL's samples, ahead
-- by the age DL measures). Aiming and the server keep the real camera.
local cvPredict = CLIENT and CreateClientConVar("gmodlight_predict", "1", true, false, "Draw from where Dying Light's camera will be when the frame is shown")
-- Turning is predicted the same way. Dying Light still re-projects the frame from
-- the predicted view to the real one, so world things stay exact; but the gun
-- (which moves with the camera and shouldn't be re-projected at all) is now only
-- shifted by the prediction's error, i.e. when a turn starts or stops, instead of
-- trailing through every turn.
local cvPredictTurn = CLIENT and CreateClientConVar("gmodlight_predict_turn", "1", true, false, "Also draw from where Dying Light's camera will be looking")
local pred = {}

-- A Source direction as Dying Light's x, y, z (the inverse of dirToSource).
function GML.DirToDL(v)
	local x = v.x
	if flipX() then x = -x end
	return x, v.z, v.y
end

function GML.RenderCamera()
	local eye, ang, fov, dl, t, latency = GML.Camera()
	if not eye or not t or t <= 0 or not cvPredict:GetBool() then return eye, ang, fov, dl end
	if pred.t and t > pred.t + 0.001 then
		local dt = t - pred.t
		local v = (dl - pred.dl) / dt
		if v:Length() > 20 then v = Vector() end  -- teleported (fast travel, respawn)
		pred.v = pred.v and LerpVector(0.5, pred.v, v) or v
		local w = Angle(math.AngleDifference(ang.p, pred.ang.p) / dt, math.AngleDifference(ang.y, pred.ang.y) / dt, 0)
		if math.abs(w.p) > 2000 or math.abs(w.y) > 2000 then w = Angle() end  -- a cut, not a turn
		pred.w = pred.w and LerpAngle(0.5, pred.w, w) or w
	end
	if pred.t ~= t then pred.t, pred.dl, pred.ang = t, dl, Angle(ang.p, ang.y, ang.r) end
	if not pred.v then return eye, ang, fov, dl end
	local ahead = math.Clamp(latency or 0, 0, 0.05)
	local p = dl + pred.v * ahead
	local a = ang
	if cvPredictTurn:GetBool() and pred.w then
		a = Angle(math.Clamp(ang.p + math.Clamp(pred.w.p * ahead, -8, 8), -89, 89), ang.y + math.Clamp(pred.w.y * ahead, -8, 8), ang.r)
	end
	return GML.ToSource(p.x, p.y, p.z), a, fov, p, ang
end

-- Tell DL which camera this frame is drawn with (position and, if predicted, view).
function GML.MarkFrame(dlPos, a, real)
	-- Not turned ahead: DL's own vectors are exact (no round trip through an Angle).
	if real and math.abs(math.AngleDifference(a.p, real.p)) + math.abs(math.AngleDifference(a.y, real.y)) < 0.005 then
		return gmodlight.MarkFrameCamera(dlPos.x, dlPos.y, dlPos.z)
	end
	local fx, fy, fz = GML.DirToDL(a:Forward())
	local ux, uy, uz = GML.DirToDL(a:Up())
	-- DL's left: forward x up, negated when DL's axes are mirrored (see GML.Camera).
	local s = GML.mirror and -1 or 1
	local lx, ly, lz = (fy * uz - fz * uy) * s, (fz * ux - fx * uz) * s, (fx * uy - fy * ux) * s
	return gmodlight.MarkFrameCamera(dlPos.x, dlPos.y, dlPos.z, fx, fy, fz, ux, uy, uz, lx, ly, lz)
end

-- A Source (4:3-based) fov as the real horizontal fov of this screen.
function GML.ScreenFov(fov43)
	local aspect = CLIENT and ScrW() / math.max(ScrH(), 1) or 4 / 3
	return math.deg(2 * math.atan(math.tan(math.rad(fov43) / 2) * aspect * 3 / 4))
end

-- ---------- server ----------

if SERVER then
	util.AddNetworkString("gml_status")
	local connected, lastTry = false, 0
	local proxies = {}  -- DL handle -> entity
	local floorZ

	local function findFloor()
		local tr = util.TraceLine({start = Vector(0, 0, 0), endpos = Vector(0, 0, -16000), mask = MASK_SOLID_BRUSHONLY})
		return tr.Hit and tr.HitPos.z or -12800
	end

	-- Dying Light's ground isn't flat, so GMod's floor can't stand for it: it's
	-- parked well below everything, and each thrown actor lands back on the
	-- height it was standing at (see syncNpcs). Re-anchoring is sideways only.
	local FLOOR_BELOW = 40  -- meters
	local function setAnchor(feetDL)
		floorZ = floorZ or findFloor()
		local oldA, oldO = anchor()
		if GML.anchored then feetDL = Vector(feetDL.x, oldA.y, feetDL.z) end
		local newO = Vector(0, 0, floorZ)
		SetGlobal2Vector("gml_anchor_dl", feetDL)
		SetGlobal2Vector("gml_origin_src", newO)
		if GML.anchored then
			-- Same DL point must stay at the same Source point for everything already spawned.
			local shift = (newO - oldO) - Vector((feetDL.x - oldA.x) * M * (flipX() and -1 or 1), (feetDL.z - oldA.z) * M, (feetDL.y - oldA.y) * M)
			for _, e in ipairs(ents.GetAll()) do
				if IsValid(e) and not e:IsPlayer() and e:GetPhysicsObject():IsValid() and not e:IsWorld() and e:CreatedByMap() == false then
					e:SetPos(e:GetPos() + shift)
				end
			end
		end
		GML.anchored = true
	end

	local function syncPlayer(ply)
		local eye, ang, _, camDL = GML.Camera()
		if not eye then return end
		local a = anchor()
		local dx, dz = camDL.x - a.x, camDL.z - a.z
		if not GML.anchored or dx * dx + dz * dz > 120 * 120 then
			-- Anchor under the player's feet; re-anchor every 120 m so coordinates stay small.
			setAnchor(Vector(camDL.x, camDL.y - FLOOR_BELOW, camDL.z))
			eye, ang = GML.Camera()
		end
		if ply:GetMoveType() ~= MOVETYPE_NOCLIP then ply:SetMoveType(MOVETYPE_NOCLIP) end
		ply:SetViewOffset(vector_origin)  -- eye == position == Dying Light camera
		ply:SetViewOffsetDucked(vector_origin)
		ply:SetPos(eye)
		ply:SetNotSolid(true)
		ply:SetNoDraw(true)
		if not ply:HasGodMode() then ply:GodEnable() end
		if not ply.GMLLoadout then
			ply.GMLLoadout = true
			if #ply:GetWeapons() < 3 then hook.Call("PlayerLoadout", GAMEMODE, ply) end
		end
		if not ply:HasWeapon("weapon_physgun") then ply:Give("weapon_physgun") end
	end

	local function syncNpcs()
		local _, list = gmodlight.GetNpcs()
		local seen = {}
		for _, npc in ipairs(list) do
			seen[npc.id] = true
			local e = proxies[npc.id]
			if not IsValid(e) then
				e = ents.Create("gml_proxy")
				e:SetPos(GML.ToSource(npc.x, npc.y, npc.z))
				e:Spawn()
				e.GMLId = npc.id
				proxies[npc.id] = e
			end
			if not e.GMLDriven then
				if npc.hx then
					-- Fit the stand-in to the pose: from between the feet to just above the
					-- head bone, tilted with the body (lying, crawling) and as long as it is.
					local feet = GML.ToSource(npc.fx, npc.fy, npc.fz)
					local head = GML.ToSource(npc.hx, npc.hy, npc.hz)
					local axis = head - feet
					local len = axis:Length()
					local up = len > 1 and axis / len or Vector(0, 0, 1)
					if up.z > 0.85 then
						-- Upright (standing or crouched): keep it vertical, it's steadier.
						up = Vector(0, 0, 1)
						len = head.z - feet.z
					end
					local yawFwd = Angle(0, math.deg(npc.yaw), 0):Forward()
					local fwd = yawFwd - up * yawFwd:Dot(up)
					if fwd:LengthSqr() < 0.01 then fwd = Vector(1, 0, 0) - up * up.x end
					fwd:Normalize()
					local m = Matrix()
					m:SetForward(fwd)
					m:SetUp(up)
					m:SetRight(fwd:Cross(up))
					e:SetPos(feet)
					e:SetAngles(m:GetAngles())
					e:SetShape(len + 0.2 * M)  -- the head bone is the base of the skull
					e.GMLHead = head + up * 0.1 * M  -- middle of the head
				else
					e:SetPos(GML.ToSource(npc.x, npc.y, npc.z))
					e:SetAngles(Angle(0, math.deg(npc.yaw), 0))
					e.GMLHead = nil
				end
			elseif bit.band(npc.flags or 0, 4) ~= 0 and not e.GMLHeld then
				-- Thrown into a Dying Light wall: stop where Dying Light stopped it.
				local phys = e:GetPhysicsObject()
				if IsValid(phys) then
					local speed = phys:GetVelocity():Length() / M
					if speed > 6 then GML.DamageActor(e, 70 * speed * 0.05, e:GetPos() + Vector(0, 0, 47), -phys:GetVelocity(), DMG_CRUSH) end
					phys:SetVelocity(vector_origin)
				end
				e:SetPos(GML.ToSource(npc.x, npc.y, npc.z))
			end
			e.GMLLastSeen = CurTime()
		end
		-- Where held stand-ins have been over the last ~0.1 s, for throwing.
		local now = CurTime()
		for _, e in pairs(proxies) do
			if IsValid(e) and e.GMLHeld then
				e.GMLTrail = e.GMLTrail or {}
				table.insert(e.GMLTrail, {pos = e:GetPos(), t = now})
				while #e.GMLTrail > 1 and now - e.GMLTrail[1].t > 0.1 do table.remove(e.GMLTrail, 1) end
			end
		end

		-- Report everything GMod is moving so Dying Light can follow.
		local driven = {}
		for id, e in pairs(proxies) do
			if not IsValid(e) then
				proxies[id] = nil
			elseif e.GMLDriven then
				local phys = e:GetPhysicsObject()
				local x, y, z = GML.ToDL(e:GetPos())
				-- Its ground is the height it stood at. Thrown, it lands there (with a
				-- thud); held, it can't be pushed below it.
				local ground = e.GMLGroundY or y
				if y < ground and IsValid(phys) then
					local fall = -phys:GetVelocity().z / M
					if not e.GMLHeld then
						if fall > 6 then GML.DamageActor(e, 70 * fall * 0.05, e:GetPos(), Vector(0, 0, -1), DMG_FALL) end
						phys:SetVelocity(vector_origin)
						phys:AddAngleVelocity(-phys:GetAngleVelocity())
					end
					y = ground
					e:SetPos(GML.ToSource(x, ground, z))
				end
				local v = IsValid(phys) and phys:GetVelocity() or vector_origin
				local vx, vy, vz = GML.DirToDL(v / M)
				driven[#driven + 1] = {id = id, x = x, y = y, z = z, vx = vx, vy = vy, vz = vz, impact = (e.GMLImpact or 0) / M}
				e.GMLImpact = 0
				-- Let go once it's thrown and has come to rest.
				if not e.GMLHeld and IsValid(phys) and phys:GetVelocity():Length() < 20 and CurTime() - (e.GMLDropTime or 0) > 1 then
					e.GMLDriven = false
					e:SetNWBool("GMLDriven", false)
					e.GMLGroundY = nil
					phys:EnableMotion(false)
				end
			elseif not seen[id] and CurTime() - (e.GMLLastSeen or 0) > 1 then
				e:Remove()
				proxies[id] = nil
			end
		end
		gmodlight.SetDriven(driven)
	end

	-- Anything GMod does to a stand-in (bullets, crowbar, explosions, thrown props)
	-- is done to the real actor in Dying Light.
	local function dirToDL(v)
		local n = v:GetNormalized()
		local x = n.x
		if flipX() then x = -x end
		return x, n.z, n.y
	end

	-- GMod damage kind -> Dying Light EDamageType (shared/bridge.h DLDamageType).
	local function dlDamageType(t)
		t = t or DMG_GENERIC
		if bit.band(t, DMG_BLAST) ~= 0 then return 3 end                          -- BLAST
		if bit.band(t, bit.bor(DMG_BULLET, DMG_BUCKSHOT, DMG_AIRBOAT)) ~= 0 then return 2 end  -- BULLET
		if bit.band(t, DMG_SLASH) ~= 0 then return 1 end                          -- CUT
		if bit.band(t, DMG_CLUB) ~= 0 then return 0xd end                         -- PUNCH2
		if bit.band(t, bit.bor(DMG_BURN, DMG_SLOWBURN)) ~= 0 then return 0xb end  -- FIRE
		if bit.band(t, DMG_SHOCK) ~= 0 then return 5 end                          -- ELECTRIC
		if bit.band(t, bit.bor(DMG_CRUSH, DMG_FALL, DMG_PHYSGUN, DMG_VEHICLE)) ~= 0 then return 0x1e end  -- IMPACT
		return 2
	end

	-- GMod keeps its own health per actor; when it runs out, Dying Light is told to
	-- kill the actor its own way. Dying Light's damage may of course get there first.
	local cvHealth = CreateConVar("gmodlight_actor_health", "35", FCVAR_ARCHIVE, "Health GMod gives each Dying Light actor")
	local cvHeadshot = CreateConVar("gmodlight_headshot_mult", "2.5", FCVAR_ARCHIVE, "Damage multiplier for hits in the top of an actor")

	function GML.DamageActor(e, amount, pos, force, gmodType)
		if not IsValid(e) or not e.GMLId or amount <= 0 or e.GMLDead then return end
		-- Head: within ~18 cm of the skeleton's head, or else the top 18% of the stand-in.
		local headshot
		if e.GMLHead and not e.GMLDriven then
			headshot = pos:Distance(e.GMLHead) < 0.18 * M
		else
			local len = e.GetBodyLength and e:GetBodyLength() or 94
			headshot = e:WorldToLocal(pos).z / math.max(len, 1) > 0.82
		end
		if headshot and bit.band(gmodType or 0, DMG_BLAST) == 0 then amount = amount * cvHeadshot:GetFloat() end
		local px, py, pz = GML.ToDL(pos)
		local dx, dy, dz = dirToDL(force:LengthSqr() > 0 and force or (pos - Entity(1):GetShootPos()))
		gmodlight.Damage(e.GMLId, amount, px, py, pz, dx, dy, dz, dlDamageType(gmodType))
		e.GMLHealth = (e.GMLHealth or cvHealth:GetFloat()) - amount
		if e.GMLHealth <= 0 then
			e.GMLDead = true
			gmodlight.Kill(e.GMLId)
			-- Corpses don't stop bullets or get grabbed.
			e:SetNotSolid(true)
			e:SetCollisionGroup(COLLISION_GROUP_DEBRIS)
			gmodlight.Log("actor " .. e.GMLId .. " killed")
		end
	end

	-- Diagnostics: where each bullet went and how close it came to a zombie box,
	-- and every few seconds where the boxes are relative to the player's eye.
	local bulletLogs = 0
	local function nearestProxy(src, dir)
		local best, bestE = math.huge, nil
		for _, e in pairs(proxies) do
			if IsValid(e) then
				local c = e:GetPos() + Vector(0, 0, 47)
				local t = math.max(0, (c - src):Dot(dir))
				local d = (src + dir * t - c):Length()
				if d < best then best, bestE = d, e end
			end
		end
		return best / M, bestE
	end
	hook.Add("EntityFireBullets", "gmodlight_diag", function(ent, data)
		if not connected or not ent:IsPlayer() or bulletLogs >= 30 then return end
		bulletLogs = bulletLogs + 1
		local tr = util.TraceLine({start = data.Src, endpos = data.Src + data.Dir * 8192, filter = ent})
		local miss, e = nearestProxy(data.Src, data.Dir:GetNormalized())
		gmodlight.Log(string.format("bullet %d: hit %s at %.1fm; nearest box %.2fm off the line (%s)",
			bulletLogs, IsValid(tr.Entity) and tr.Entity:GetClass() or (tr.HitWorld and "world" or "nothing"),
			tr.Fraction * 8192 / M, miss, IsValid(e) and e.GMLId or "-"))
	end)
	-- ---------- Dying Light's world for GMod things ----------
	-- GMod has no walls or ground here (its map is 40 m down, hidden), so rockets
	-- flew forever and grenades fell through the street. Every tick each projectile's
	-- path is sent to Dying Light, which traces it through its own world; where it
	-- hits, rockets explode, grenades bounce, bolts stick. Bullets get an impact puff.
	local PROJECTILES = {
		rpg_missile = "explode", grenade_ar2 = "explode",
		npc_grenade_frag = "bounce", prop_combine_ball = "bounce",
		crossbow_bolt = "stick",
	}
	local tracked = {}       -- entity -> {last = pos}
	local probeOwner = {}    -- probe id -> {ent = e} or {bullet = true}, until answered
	local pendingBullets = {}
	local nextProbe, lastHitSeq, sentAny = 1, 0, false

	hook.Add("OnEntityCreated", "gmodlight_world", function(e)
		if not connected or not IsValid(e) or not PROJECTILES[e:GetClass()] then return end
		timer.Simple(0, function() if IsValid(e) then tracked[e] = {last = e:GetPos()} end end)
	end)

	hook.Add("EntityFireBullets", "gmodlight_world", function(ent, data)
		if not connected or not ent:IsPlayer() then return end
		if #pendingBullets < 8 then
			-- Only as far as a zombie it hits: no spark on the wall behind one.
			local to = data.Src + data.Dir:GetNormalized() * 100 * M
			local tr = util.TraceLine({start = data.Src, endpos = to, filter = ent})
			if IsValid(tr.Entity) and tr.Entity:GetClass() == "gml_proxy" then to = tr.HitPos end
			pendingBullets[#pendingBullets + 1] = {from = data.Src, to = to}
		end
	end)

	local function explode(e, pos, normal)
		local owner = e:GetOwner()
		if not IsValid(owner) then owner = Entity(1) end
		local big = e:GetClass() == "rpg_missile"
		e:Remove()
		local ex = ents.Create("env_explosion")
		ex:SetPos(pos + normal * 8)
		ex:SetOwner(owner)
		ex:SetKeyValue("iMagnitude", big and "150" or "100")
		ex:Spawn()
		ex:Fire("Explode", "", 0)
		SafeRemoveEntityDelayed(ex, 2)
	end

	local function bounce(e, pos, normal)
		local phys = e:GetPhysicsObject()
		if not IsValid(phys) then return end
		local v = phys:GetVelocity()
		local into = v:Dot(normal)
		if into >= 0 then return end
		v = (v - normal * into * 1.45) * 0.75  -- lose most of the bounce, some of the slide
		e:SetPos(pos + normal * 5)  -- a frag grenade is ~4 units round
		if tracked[e] then tracked[e].last = e:GetPos() end  -- next path starts above the ground
		if v:Length() < 60 then
			phys:SetVelocity(vector_origin)
			phys:EnableMotion(false)  -- at rest on Dying Light's ground (a frag still goes off)
		else
			phys:SetVelocity(v)
		end
		if into < -150 then e:EmitSound("physics/metal/metal_grenade_impact_hard" .. math.random(1, 3) .. ".wav") end
	end

	local function impact(pos, normal)
		local ed = EffectData()
		ed:SetOrigin(pos)
		ed:SetNormal(normal)
		ed:SetMagnitude(1)
		ed:SetScale(1)
		ed:SetRadius(2)
		util.Effect("MetalSpark", ed)
		sound.Play("physics/concrete/concrete_impact_bullet" .. math.random(1, 4) .. ".wav", pos, 70, math.random(95, 105), 0.6)
	end

	local worldLogs = 0
	local function traceWorld()
		local probes, now = {}, CurTime()
		local function add(owner, from, to)
			if #probes >= 32 then return end
			local id = nextProbe
			nextProbe = nextProbe % 1000000 + 1
			owner.at = now
			probeOwner[id] = owner
			local fx, fy, fz = GML.ToDL(from)
			local tx, ty, tz = GML.ToDL(to)
			probes[#probes + 1] = {id, fx, fy, fz, tx, ty, tz}
		end
		for e, t in pairs(tracked) do
			if not IsValid(e) then
				tracked[e] = nil
			else
				local pos = e:GetPos()
				local phys = e:GetPhysicsObject()
				local v = IsValid(phys) and phys:GetVelocity() or e:GetVelocity()
				-- From last tick's position to three ticks ahead: Dying Light answers a tick or
				-- two later, and by then a grenade had already sunk into the ground.
				if v:LengthSqr() > 1 then add({ent = e}, t.last, pos + v * engine.TickInterval() * 3) end
				t.last = pos
			end
		end
		for _, b in ipairs(pendingBullets) do add({bullet = true}, b.from, b.to) end
		pendingBullets = {}
		if #probes > 0 or sentAny then
			gmodlight.SetProbes(probes)
			sentAny = #probes > 0
		end

		local seq, hits = gmodlight.GetProbeHits()
		if seq ~= lastHitSeq then
			lastHitSeq = seq
			for _, h in ipairs(hits) do
				local owner = probeOwner[h.id]
				if owner and not owner.done then
					owner.done = true
					local pos = GML.ToSource(h.x, h.y, h.z)
					local normal = dirToSource(h.nx, h.ny, h.nz)
					if normal:LengthSqr() < 0.25 then normal = Vector(0, 0, 1) else normal:Normalize() end
					local e = owner.ent
					if owner.bullet then
						impact(pos, normal)
					elseif IsValid(e) then
						local what = PROJECTILES[e:GetClass()]
						if worldLogs < 20 then
							worldLogs = worldLogs + 1
							gmodlight.Log(string.format("%s hit Dying Light's world %.1fm along its path: %s", e:GetClass(), h.dist, what))
						end
						if what == "explode" then
							tracked[e] = nil
							explode(e, pos, normal)
						elseif what == "bounce" then
							bounce(e, pos, normal)
						elseif what == "stick" then
							tracked[e] = nil
							e:SetPos(pos + normal * 2)
							e:SetMoveType(MOVETYPE_NONE)
							impact(pos, normal)
							SafeRemoveEntityDelayed(e, 15)
						end
					end
				end
			end
		end
		for id, o in pairs(probeOwner) do
			if now - o.at > 1 then probeOwner[id] = nil end
		end
	end
	GML.TraceWorld = traceWorld

	local lastBoxReport, boxReports = 0, 0
	hook.Add("Think", "gmodlight_diag", function()
		if not connected or gmodlight.GetMode() == 0 or boxReports >= 12 or CurTime() - lastBoxReport < 5 then return end
		lastBoxReport = CurTime()
		boxReports = boxReports + 1
		local ply = Entity(1)
		if not IsValid(ply) then return end
		local ex, ey, ez = GML.ToDL(ply:GetShootPos())
		local n = 0
		for _ in pairs(proxies) do n = n + 1 end
		gmodlight.Log(string.format("boxes: %d, eye in DL (%.2f, %.2f, %.2f), aim %s, mirror %s", n, ex, ey, ez,
			tostring(ply:GetAimVector()), tostring(GML.mirror)))
		for id, e in pairs(proxies) do
			if IsValid(e) then
				local x, y, z = GML.ToDL(e:GetPos())
				gmodlight.Log(string.format("  box %s at DL (%.2f, %.2f, %.2f), %.1fm from eye%s", id, x, y, z,
					(e:GetPos() - ply:GetShootPos()):Length() / M, e.GMLDriven and ", held/flying" or ""))
			end
		end
	end)

	hook.Add("EntityTakeDamage", "gmodlight", function(e, dmg)
		if e:GetClass() ~= "gml_proxy" then return end
		GML.DamageActor(e, dmg:GetDamage(), dmg:GetDamagePosition(), dmg:GetDamageForce(), dmg:GetDamageType())
		return true  -- the stand-in itself never takes damage
	end)

	hook.Add("PhysgunPickup", "gmodlight", function(ply, e)
		if e.GMLDead then return false end
	end)

	hook.Add("OnPhysgunPickup", "gmodlight", function(ply, e)
		if e:GetClass() ~= "gml_proxy" then return end
		e.GMLDriven, e.GMLHeld = true, true
		local _, gy = GML.ToDL(e:GetPos())
		e.GMLGroundY = e.GMLGroundY or gy
		e:SetNWBool("GMLDriven", true)
		local phys = e:GetPhysicsObject()
		if IsValid(phys) then phys:EnableMotion(true) phys:Wake() end
		if gmodlight.SelfTest() then
			timer.Create("gml_holdlog", 0.25, 24, function()
				if not IsValid(e) or not e.GMLHeld then return end
				local p = e:GetPhysicsObject()
				gmodlight.Log(string.format("held: pos %s vel %s | eye %s aim %s", tostring(e:GetPos()),
					tostring(IsValid(p) and p:GetVelocity()), tostring(ply:GetShootPos()), tostring(ply:GetAimVector())))
			end)
		end
	end)

	hook.Add("PhysgunDrop", "gmodlight", function(ply, e)
		if e:GetClass() ~= "gml_proxy" then return end
		e.GMLHeld = false
		e.GMLDropTime = CurTime()
		-- Throw it the way it was actually moving while held (a flick throws, a
		-- still release drops). The physgun's own release velocity was wild when
		-- the target was pressed into the floor.
		local trail = e.GMLTrail or {}
		local v = vector_origin
		if #trail >= 2 then
			local a, b = trail[1], trail[#trail]
			if b.t - a.t > 0.01 then v = (b.pos - a.pos) / (b.t - a.t) end
		end
		if v:Length() > 3000 then v = v:GetNormalized() * 3000 end
		e.GMLTrail = nil
		timer.Simple(0, function()
			local phys = IsValid(e) and e:GetPhysicsObject()
			if not IsValid(phys) then return end
			phys:SetVelocity(v)
			phys:AddAngleVelocity(-phys:GetAngleVelocity())
		end)
		if gmodlight.SelfTest() then
			for _, dt in ipairs({0, 0.05, 0.15, 0.4}) do
				timer.Simple(dt, function()
					if not IsValid(e) then return end
					local phys = e:GetPhysicsObject()
					gmodlight.Log(string.format("drop +%.2fs: pos %s vel %s angvel %s motion %s", dt, tostring(e:GetPos()),
						tostring(IsValid(phys) and phys:GetVelocity()), tostring(IsValid(phys) and phys:GetAngleVelocity()),
						tostring(IsValid(phys) and phys:IsMotionEnabled())))
				end)
			end
		end
	end)

	-- gm_flatgrass's own effects (sun glow, sprites, dust) would float over Dying
	-- Light; nothing of the map should show.
	local MAP_EFFECTS = {
		env_sun = true, env_sprite = true, env_lightglow = true, env_smokestack = true,
		func_dustcloud = true, func_dustmotes = true, func_smokevolume = true, env_steam = true,
	}
	local function removeMapEffects()
		local found = {}
		for _, e in ipairs(ents.GetAll()) do
			if IsValid(e) and e:CreatedByMap() then
				local c = e:GetClass()
				found[c] = (found[c] or 0) + 1
				if MAP_EFFECTS[c] then e:Remove() end
			end
		end
		local list = {}
		for c, n in pairs(found) do list[#list + 1] = c .. " x" .. n end
		gmodlight.Log("map entities: " .. table.concat(list, ", "))
	end

	hook.Add("Think", "gmodlight", function()
		if not connected then
			if CurTime() - lastTry < 2 then return end
			lastTry = CurTime()
			connected = gmodlight.Connect()
			if connected then
				gmodlight.Log("server connected")
				removeMapEffects()
			end
			return
		end
		if not gmodlight.Heartbeat() then return end
		local ply = Entity(1)
		if IsValid(ply) then syncPlayer(ply) end
		if GML.anchored then
			syncNpcs()
			GML.TraceWorld()
		end
	end)

	-- tools/fakehost: a few props 4 m in front of the (fixed) test camera.
	if gmodlight.SelfTest() then
		hook.Add("Think", "gmodlight_selftest", function()
			if not GML.anchored then return end
			hook.Remove("Think", "gmodlight_selftest")
			RunConsoleCommand("gmodlight_debug", "1")
			local models = {"models/props_c17/oildrum001.mdl", "models/props_junk/wood_crate001a.mdl", "models/props_c17/furnituretable001a.mdl"}
			for i, m in ipairs(models) do
				local e = ents.Create("prop_physics")
				e:SetModel(m)
				local _, _, _, cam = GML.Camera()
				e:SetPos(GML.ToSource(cam.x + 4, cam.y - 1.1, cam.z + (i == 2 and 4 or (i - 2) * 3)))  -- clear of the test zombie's path
				e:Spawn()
				-- Held in place (GMod's own ground is 40 m down): known distances for depth tests.
				local phys = e:GetPhysicsObject()
				if IsValid(phys) then phys:EnableMotion(false) end
			end
			gmodlight.Log("selftest: spawned props")
			-- A rocket into the ground 16 m away, and a grenade lobbed off to the side to
			-- bounce: both need Dying Light's world (probes). Both well clear of the test
			-- zombie, which the physgun and weapon tests still need alive.
			timer.Simple(6, function()
				local _, _, _, cam = GML.Camera()
				local from, to = GML.ToSource(cam.x + 1, cam.y - 0.3, cam.z - 2), GML.ToSource(cam.x + 12, cam.y - 1.7, cam.z - 12)
				local m = ents.Create("rpg_missile")
				m:SetPos(from)
				m:SetAngles((to - from):Angle())
				m:SetOwner(Entity(1))
				m:Spawn()
				m:SetVelocity((to - from):GetNormalized() * 1500)
				gmodlight.Log("selftest: rocket fired")
			end)
			timer.Simple(10, function()
				local _, _, _, cam = GML.Camera()
				local g = ents.Create("npc_grenade_frag")
				g:SetPos(GML.ToSource(cam.x + 0.5, cam.y - 0.2, cam.z - 1))
				g:SetOwner(Entity(1))
				g:Spawn()
				g:Fire("SetTimer", "2.5")
				local phys = g:GetPhysicsObject()
				if IsValid(phys) then phys:SetVelocity(GML.ToSource(cam.x + 1.2, cam.y + 1.5, cam.z - 9) - GML.ToSource(cam.x, cam.y, cam.z)) end
				gmodlight.Log("selftest: grenade thrown")
			end)
		end)
	end

	-- Hidden GMod has nobody to press keys; never let it pause or die.
	hook.Add("PlayerShouldTakeDamage", "gmodlight", function() if connected then return false end end)
	return
end

-- ---------- client ----------

local connected, ready, lastTry = false, false, 0
local mode = MODE_PLAY
local menuCmd = nil           -- "menu" or "menu_context" while one is held open
local held = {}               -- GMod input commands we're holding (+attack, +reload, ...)
local pendingWheel = 0        -- physgun push/pull, handed over in CreateMove
local cursorX, cursorY = 0, 0 -- menu cursor, as sent by Dying Light
local lastWeapon

local VK_TO_KEY = {
	[8] = KEY_BACKSPACE, [9] = KEY_TAB, [13] = KEY_ENTER, [27] = KEY_ESCAPE, [32] = KEY_SPACE,
	[37] = KEY_LEFT, [38] = KEY_UP, [39] = KEY_RIGHT, [40] = KEY_DOWN, [46] = KEY_DELETE,
	[160] = KEY_LSHIFT, [161] = KEY_RSHIFT, [162] = KEY_LCONTROL, [163] = KEY_RCONTROL, [35] = KEY_END, [36] = KEY_HOME,
}
for i = 0, 25 do VK_TO_KEY[65 + i] = KEY_A + i end
for i = 0, 9 do VK_TO_KEY[48 + i] = KEY_0 + i end
local MOUSE_CODES = {[0] = MOUSE_LEFT, [1] = MOUSE_RIGHT, [2] = MOUSE_MIDDLE}

local function isMenu(m) return m == MODE_MENU or m == MODE_CONTEXT end

-- GMod's own commands, exactly what its key binds run, so weapon selection,
-- the physgun, the toolgun and everything else behave like real GMod.
local function press(cmd)
	if held[cmd] then return end
	held[cmd] = true
	RunConsoleCommand("+" .. cmd)
end

local function release(cmd)
	if not held[cmd] then return end
	held[cmd] = nil
	RunConsoleCommand("-" .. cmd)
end

local function releaseAll()
	for cmd in pairs(held) do RunConsoleCommand("-" .. cmd) end
	held = {}
end

local MOUSE_CMD = {[0] = "attack", [1] = "attack2"}
local HELD_KEY_CMD = {[82] = "reload", [69] = "use"}  -- R, E

local function physgunHolding()
	local ply = LocalPlayer()
	local wep = IsValid(ply) and ply:GetActiveWeapon()
	return held.attack and IsValid(wep) and wep:GetClass() == "weapon_physgun"
end

-- GMod hands: buttons, wheel and keys become GMod commands.
local function handsInput(t, a)
	if t == 2 and MOUSE_CMD[a] then press(MOUSE_CMD[a])
	elseif t == 3 and MOUSE_CMD[a] then release(MOUSE_CMD[a])
	elseif t == 4 then
		if physgunHolding() then
			pendingWheel = pendingWheel + a  -- pushes/pulls what the physgun holds
		else
			RunConsoleCommand(a > 0 and "invprev" or "invnext")  -- weapon wheel
		end
	elseif t == 5 then
		if a >= 48 and a <= 57 then RunConsoleCommand("slot" .. (a == 48 and 10 or a - 48))
		elseif HELD_KEY_CMD[a] then press(HELD_KEY_CMD[a])
		elseif a == 90 then RunConsoleCommand("gmod_undo")  -- Z
		end
	elseif t == 6 and HELD_KEY_CMD[a] then release(HELD_KEY_CMD[a])
	end
end

-- gui.Internal* feed VGUI as if the OS sent the event. Not every build has every one.
local function vgui_(name, ...)
	local f = gui[name]
	if f then f(...) end
end

local function menuInput(t, a, b)
	if t == 1 then cursorX, cursorY = a, b vgui_("InternalCursorMoved", a, b)
	elseif t == 2 then vgui_("InternalMousePressed", MOUSE_CODES[a] or MOUSE_LEFT)
	elseif t == 3 then vgui_("InternalMouseReleased", MOUSE_CODES[a] or MOUSE_LEFT)
	elseif t == 4 then vgui_("InternalMouseWheeled", a)
	elseif t == 5 and VK_TO_KEY[a] then vgui_("InternalKeyCodePressed", VK_TO_KEY[a]) vgui_("InternalKeyCodeTyped", VK_TO_KEY[a])
	elseif t == 6 and VK_TO_KEY[a] then vgui_("InternalKeyCodeReleased", VK_TO_KEY[a])
	elseif t == 7 and a >= 32 then vgui_("InternalKeyTyped", a)
	end
end

local function setMode(m)
	local old = mode
	mode = m
	local wantMenu = (m == MODE_MENU and "menu") or (m == MODE_CONTEXT and "menu_context") or nil
	if menuCmd ~= wantMenu then
		if menuCmd then RunConsoleCommand("-" .. menuCmd) end
		if wantMenu then RunConsoleCommand("+" .. wantMenu) end
		menuCmd = wantMenu
	end
	if m ~= MODE_GMOD then releaseAll() end
	pendingWheel = 0
	if m == MODE_GMOD and old == MODE_PLAY and not lastWeapon then
		-- First time in GMod hands: start with the physgun out.
		local ply = LocalPlayer()
		local wep = IsValid(ply) and ply:GetWeapon("weapon_physgun")
		if IsValid(wep) then input.SelectWeapon(wep) end
	end
	gmodlight.Log("mode " .. old .. " -> " .. m)
end

hook.Add("Think", "gmodlight", function()
	if not connected then
		if RealTime() - lastTry < 2 then return end
		lastTry = RealTime()
		connected = gmodlight.Connect()
		if not connected then return end
		local capture = gmodlight.StartCapture()
		gmodlight.Log("client connected; capture hook " .. tostring(capture))
		-- Lua-made convars don't exist yet when the command line runs, so set them here.
		RunConsoleCommand("cl_showhints", "0")
		if gmodlight.SelfTest() then
			timer.Create("gmodlight_stats", 4, 0, function() gmodlight.Log(gmodlight.Stats()) end)
		end
		timer.Simple(3, function() gmodlight.HideWindow() end)
	end
	if not gmodlight.Heartbeat() then return end
	if not ready and IsValid(LocalPlayer()) and GML.Camera() then
		ready = true
		gmodlight.SetReady(true)
		gmodlight.Log("client ready; screen " .. ScrW() .. "x" .. ScrH() .. ", interpolation " .. math.Round(GetConVar("cl_interp"):GetFloat() * 1000) .. " ms (ratio " .. GetConVar("cl_interp_ratio"):GetFloat() .. ", update rate " .. GetConVar("cl_updaterate"):GetFloat() .. ")")
	end

	GML.debugBoxes = bit.band(gmodlight.GetDebugFlags(), 1) ~= 0 or GetConVar("gmodlight_debug"):GetBool()
	local newMode = gmodlight.GetMode()
	if newMode ~= mode then setMode(newMode) end
	for _, ev in ipairs(gmodlight.PollInput()) do
		if isMenu(mode) then menuInput(ev.type, ev.a, ev.b)
		elseif mode == MODE_GMOD then handsInput(ev.type, ev.a)
		end
	end

	local ply = LocalPlayer()
	local wep = IsValid(ply) and ply:GetActiveWeapon()
	local class = IsValid(wep) and wep:GetClass() or "none"
	if class ~= lastWeapon then
		lastWeapon = class
		gmodlight.Log("weapon: " .. class)
	end
end)

-- Aim from the Dying Light camera. Movement is Dying Light's; buttons come from
-- the commands above, so they're left alone here.
hook.Add("CreateMove", "gmodlight", function(cmd)
	if not connected then return end
	local eye, ang = GML.Camera()
	if not eye then return end
	cmd:SetViewAngles(ang)
	cmd:ClearMovement()
	if pendingWheel ~= 0 then
		cmd:SetMouseWheel(pendingWheel)
		pendingWheel = 0
	end
end)

hook.Add("CalcView", "gmodlight", function(ply, origin, angles, fov)
	if not connected then return end
	local eye, ang, srcFov = GML.RenderCamera()
	if not eye then return end
	return {origin = eye, angles = ang, fov = srcFov, drawviewer = false}
end)

-- The viewmodel used to be bolted to Dying Light's camera. GMod's own sway and
-- bob come from the player's movement, and GMod's player doesn't move (it's set
-- to the camera every tick), so they're made here from how Dying Light's camera
-- moves: the gun trails a little when turning, carries some inertia when running,
-- jumping and landing (a sprung offset, so it settles with a small bounce) and
-- bobs while walking. Small on purpose: Dying Light's camera has its own head bob.
local cvSway = CreateClientConVar("gmodlight_vm_sway", "1", true, false, "Weapon sway and bob from Dying Light's movement (0 = rigid)")
local vmm = {off = Vector(), offVel = Vector(), lag = Angle(), phase = 0}

local function angDelta(a, b)
	return Angle(math.AngleDifference(a.p, b.p), math.AngleDifference(a.y, b.y), 0)
end

hook.Add("CalcViewModelView", "gmodlight", function(wep, vm, oldPos, oldAng, pos, ang)
	if not connected then return end
	-- Called per viewmodel (and hands) each frame; move once per frame.
	if vmm.frame == FrameNumber() and vmm.outPos then return vmm.outPos, vmm.outAng end
	local eye, a = GML.RenderCamera()
	if not eye then return end
	vmm.frame = FrameNumber()
	local amount = cvSway:GetFloat()
	local dt = math.Clamp(RealFrameTime(), 0.001, 0.05)
	if amount <= 0 or not vmm.eye or (eye - vmm.eye):LengthSqr() > (5 * M) ^ 2 then
		-- First frame, sway off, or a teleport (fast travel, respawn): start still.
		vmm.eye, vmm.ang = eye, a
		vmm.off, vmm.offVel, vmm.lag, vmm.lvSlow = Vector(), Vector(), Angle(), nil
		vmm.outPos, vmm.outAng = eye, a
		return eye, a
	end
	local fwd, right, up = a:Forward(), a:Right(), a:Up()
	local v = (eye - vmm.eye) / dt
	local lv = Vector(v:Dot(fwd), v:Dot(right), v:Dot(up))  -- camera space, units/s
	local turn = angDelta(a, vmm.ang) / dt                   -- degrees/s
	vmm.eye, vmm.ang = eye, a

	-- Inertia: the gun is pushed against *changes* in motion (starting, stopping,
	-- jumping, landing), on a spring (slightly bouncy). Not against motion itself:
	-- that held it back the whole time you ran, which looked like GMod falling behind.
	vmm.lvSlow = vmm.lvSlow and LerpVector(1 - math.exp(-dt / 0.25), vmm.lvSlow, lv) or lv
	local dv = lv - vmm.lvSlow  -- how much faster (or slower) than a moment ago
	local target = Vector(math.Clamp(-dv.x * 0.004, -2, 2), math.Clamp(-dv.y * 0.003, -1.5, 1.5), math.Clamp(-dv.z * 0.004, -2.5, 2.5))
	local k, c = 110, 13
	vmm.offVel = vmm.offVel + ((target - vmm.off) * k - vmm.offVel * c) * dt
	vmm.off = vmm.off + vmm.offVel * dt

	-- Sway: trails the turn by a few degrees at most.
	local lagTarget = Angle(math.Clamp(-turn.p * 0.012, -3, 3), math.Clamp(-turn.y * 0.012, -4, 4), 0)
	local s = 1 - math.exp(-dt * 12)
	vmm.lag = vmm.lag + (lagTarget - vmm.lag) * s

	-- Bob while walking/running on the ground.
	local hspeed = math.sqrt(lv.x * lv.x + lv.y * lv.y) / M  -- m/s
	local bobAmp = 0
	if hspeed > 0.5 and math.abs(lv.z) < 2 * M then
		vmm.phase = vmm.phase + dt * (5 + hspeed * 0.8)
		bobAmp = math.min(hspeed / 7, 1) * 0.45
	end
	local bobX, bobZ = math.sin(vmm.phase) * bobAmp, -math.abs(math.cos(vmm.phase)) * bobAmp * 0.6

	-- Viewmodels end a little below and behind the screen edge (the arms are cut
	-- off there). Moving the gun up or forward, or tilting it up, shows that cut
	-- edge: the "gap at the bottom". So it may dip, pull back and swing sideways
	-- freely, but only barely rise, push forward or tilt up.
	local o = vmm.off * amount
	o.x = math.min(o.x, 0.4)
	o.z = math.min(o.z, 0.25)
	local p = eye + fwd * o.x + right * (o.y + bobX * amount) + up * (o.z + bobZ * amount)
	local l = vmm.lag * amount
	l.p = math.max(l.p, -0.4)  -- negative pitch = tilted up
	local outAng = Angle(a.p + l.p, a.y + l.y, a.r - l.y * 0.4)
	vmm.outPos, vmm.outAng = p, outAng
	return p, outAng
end)

-- On top of that, what the weapon itself does with its viewmodel (scripted
-- weapons offset it with GetViewModelPosition/CalcViewModelView), like GMod's own
-- CalcViewModelView. Applied per weapon, after the shared motion above.
local baseCalc = hook.GetTable().CalcViewModelView.gmodlight  -- the shared motion, above
if baseCalc then
	hook.Add("CalcViewModelView", "gmodlight", function(wep, vm, oldPos, oldAng, pos, ang)
		local p, a = baseCalc(wep, vm, oldPos, oldAng, pos, ang)
		if not p or not IsValid(wep) then return p, a end
		p, a = Vector(p), Angle(a)
		if wep.GetViewModelPosition then
			local np, na = wep:GetViewModelPosition(Vector(p), Angle(a))
			p, a = np or p, na or a
		end
		if wep.CalcViewModelView then
			local np, na = wep:CalcViewModelView(vm, oldPos, oldAng, Vector(p), Angle(a))
			p, a = np or p, na or a
		end
		return p, a
	end)
end

-- ---------- distances for Dying Light ----------
-- Dying Light re-projects GMod's (~30 ms old) frame to its current camera. Rotation
-- alone isn't enough: the eye also moves (head pivot, bob, walking), and near
-- things (a grenade at your feet) visibly slid with the camera. So the frame's
-- alpha carries how far away things are: each world object's screen rectangle is
-- filled with its distance (nearest drawn last); everything else, including the gun
-- and hands (which move with the camera), is 0 = unknown, so it's never shifted.
-- Colour is untouched. Decoded in dl_plugin/src/overlay.cpp (kDepthMax there).
GML.DEPTH_MAX = 25  -- meters at alpha 254
-- Alpha 8..254 = 0..DEPTH_MAX meters. Below 8 and 255 = unknown (Source leaves small
-- values around after our pass).
function GML.EncodeDepth(meters)
	return math.Clamp(8 + math.Round(meters / GML.DEPTH_MAX * 246), 8, 254)
end
local cvDepth = CreateClientConVar("gmodlight_depth", "1", true, false, "Send distances so Dying Light can correct for head movement")
local depthWhite = Material("models/debug/debugwhite")
local DEPTH_SKIP_PREFIX = {"env_", "info_", "point_", "func_", "light", "logic_", "trigger_", "gmod_hands", "predicted_viewmodel", "viewmodel"}

local function depthSkip(e)
	local c = e:GetClass()
	for _, p in ipairs(DEPTH_SKIP_PREFIX) do
		if c:sub(1, #p) == p then return true end
	end
	return false
end

function GML.DrawDepthAlpha(eye, ang, hfov, vmFov)
	-- Off: alpha is still cleared below, so Dying Light never reads leftovers as distances.
	local on = cvDepth:GetBool()
	local w, h = ScrW(), ScrH()
	local tx = math.tan(math.rad(hfov) / 2)
	local ty = tx * h / w
	local fwd, right, up = ang:Forward(), ang:Right(), ang:Up()
	local ply = LocalPlayer()
	local vm, hands = ply:GetViewModel(), ply:GetHands()
	local rects = {}
	for _, e in ipairs(on and ents.GetAll() or {}) do
		if IsValid(e) and not e:IsWorld() and not e:IsPlayer() and not e:IsWeapon() and e ~= vm and e ~= hands
			and not e:GetNoDraw() and e:GetModel() and not depthSkip(e) then
			local mins, maxs = e:GetRenderBounds()
			local z = (e:LocalToWorld((mins + maxs) * 0.5) - eye):Dot(fwd)
			if z > 4 and z < GML.DEPTH_MAX * M then
				local x0, y0, x1, y1 = math.huge, math.huge, -math.huge, -math.huge
				local ok = true
				for i = 0, 7 do
					local p = e:LocalToWorld(Vector(bit.band(i, 1) ~= 0 and maxs.x or mins.x,
						bit.band(i, 2) ~= 0 and maxs.y or mins.y, bit.band(i, 4) ~= 0 and maxs.z or mins.z))
					local v = p - eye
					local vz = v:Dot(fwd)
					if vz < 1 then ok = false break end  -- reaches behind the eye: leave it to rotation
					local sx = w / 2 + v:Dot(right) / vz / tx * w / 2
					local sy = h / 2 - v:Dot(up) / vz / ty * h / 2
					x0, y0, x1, y1 = math.min(x0, sx), math.min(y0, sy), math.max(x1, sx), math.max(y1, sy)
				end
				if ok and x1 > 0 and y1 > 0 and x0 < w and y0 < h then
					rects[#rects + 1] = {z = z, x0 = x0, y0 = y0, x1 = x1, y1 = y1}
				end
			end
		end
	end
	table.sort(rects, function(a, b) return a.z > b.z end)  -- far first: near ones win where they overlap

	render.OverrideColorWriteEnable(true, false)
	render.OverrideAlphaWriteEnable(true, true)
	render.OverrideBlend(true, BLEND_ONE, BLEND_ZERO, BLENDFUNC_ADD, BLEND_ONE, BLEND_ZERO, BLENDFUNC_ADD)
	cam.Start2D()
	draw.NoTexture()
	-- Whatever Source left in alpha (its own depth or texture alpha) means nothing
	-- here: start from 0 = "unknown" everywhere.
	surface.SetDrawColor(0, 0, 0, 0)
	surface.DrawRect(0, 0, w, h)
	for _, r in ipairs(rects) do
		surface.SetDrawColor(0, 0, 0, GML.EncodeDepth(r.z / M))
		surface.DrawRect(math.floor(r.x0) - 2, math.floor(r.y0) - 2, math.ceil(r.x1 - r.x0) + 4, math.ceil(r.y1 - r.y0) + 4)
	end
	cam.End2D()
	if #rects > 0 and mode ~= MODE_PLAY and (IsValid(vm) or IsValid(hands)) then
		-- Gun and hands back to 0 ("unknown": rotation only, never shifted). The blend
		-- forces alpha to 0 whatever the material's shader outputs there.
		render.OverrideBlend(true, BLEND_ZERO, BLEND_ONE, BLENDFUNC_ADD, BLEND_ZERO, BLEND_ZERO, BLENDFUNC_ADD)
		cam.Start3D(eye, ang, vmFov or hfov, 0, 0, w, h, 1, 2000)
		cam.IgnoreZ(true)
		render.MaterialOverride(depthWhite)
		if IsValid(vm) then vm:DrawModel() end
		if IsValid(hands) then hands:DrawModel() end
		render.MaterialOverride()
		cam.IgnoreZ(false)
		cam.End3D()
	end
	render.OverrideBlend(false)
	render.OverrideAlphaWriteEnable(false)
	render.OverrideColorWriteEnable(false)
end

-- Everything Dying Light shouldn't show is cleared to the chroma key. Dying Light
-- un-mixes translucent pixels from it, so menus can stay see-through like in GMod.
hook.Add("PreDrawSkyBox", "gmodlight", function() if connected then return true end end)
-- Draw the scene ourselves on top of a magenta clear. (With the world off, the
-- engine skips the opaque-renderables hooks, so there's nowhere later to clear.)
local inScene, loggedScene = false, false
hook.Add("RenderScene", "gmodlight", function(origin, angles, fov)
	if not connected or inScene then return end
	render.Clear(255, 0, 255, 255, true, true)
	local eye, ang, srcFov, dlp, realAng = GML.RenderCamera()
	if not eye then return true end  -- no Dying Light camera (loading, menus): draw nothing
	local frameId = GML.MarkFrame(dlp, ang, realAng)  -- the camera this frame is drawn with, for re-projection in DL
	inScene = true
	-- CalcView's fov is 4:3-based and the engine widens it to the screen, but
	-- render.RenderView takes the real horizontal fov. Passing the 4:3 one made
	-- everything ~1.8x zoomed on a 21:9 screen (boxes swung twice as far as DL's
	-- view, guns filled the screen). The viewmodel needs the same conversion.
	local fov, vmFov = srcFov, nil
	if cvScaleRenderFov:GetBool() then
		fov = GML.ScreenFov(srcFov)
		local wep = LocalPlayer():GetActiveWeapon()
		local base = IsValid(wep) and tonumber(wep.ViewModelFOV) or GetConVar("viewmodel_fov"):GetFloat()
		vmFov = GML.ScreenFov(base * cvViewmodelFov:GetFloat())
	end
	render.RenderView({
		origin = eye, angles = ang, fov = fov, viewmodelfov = vmFov,
		x = 0, y = 0, w = ScrW(), h = ScrH(),
		drawhud = true, drawviewmodel = mode ~= MODE_PLAY, drawmonitors = false,
		dopostprocess = false,
	})
	GML.DrawDepthAlpha(eye, ang, fov, vmFov)
	inScene = false
	if frameId then
		-- Which frame this image is, for the capture to attach the camera it was really
		-- drawn with (images reach Present a frame early or late). Painted over with the
		-- key before Dying Light sees it.
		local id = frameId % 16
		cam.Start2D()
		for k = 0, 4 do
			local on = k == 0 or bit.band(id, bit.lshift(1, k - 1)) ~= 0
			surface.SetDrawColor(on and 255 or 0, 0, 0, 255)
			surface.DrawRect(k * 4, 0, 4, 4)
		end
		cam.End2D()
	end
	if not loggedScene then
		loggedScene = true
		gmodlight.Log(string.format("RenderScene override active; fov %.1f (4:3 %.1f), viewmodel fov %s", fov, srcFov, tostring(vmFov)))
	end
	return true
end)

-- GMod's weapons are lit by gm_flatgrass's sun, so at night in Dying Light they
-- glowed. Dying Light sends how bright and what colour its picture is; the
-- viewmodel and hands are lit to match (a dim, cool light at night, full in daylight).
local cvMatchLight = CreateClientConVar("gmodlight_match_lighting", "1", true, false, "Light GMod's weapons like Dying Light's scene")
local cvLightScale = CreateClientConVar("gmodlight_light_scale", "1", true, false, "Brightness of GMod's weapons relative to Dying Light's scene")
local scene = {r = 1, g = 1, b = 1, have = false}
local litNow = false

hook.Add("Think", "gmodlight_scenelight", function()
	if not connected then return end
	local r, g, b, seq = gmodlight.GetSceneLight()
	if not seq or seq == 0 then return end
	if not scene.have then
		scene.r, scene.g, scene.b, scene.have = r, g, b, true
		gmodlight.Log(string.format("scene light: %.3f %.3f %.3f", r, g, b))
		return
	end
	local k = 1 - math.exp(-FrameTime() * 2)  -- eases over about half a second, so muzzle flashes don't flicker it
	scene.r = scene.r + (r - scene.r) * k
	scene.g = scene.g + (g - scene.g) * k
	scene.b = scene.b + (b - scene.b) * k
end)

local function beginSceneLight()
	if not connected or not scene.have or not cvMatchLight:GetBool() then return end
	local r, g, b = scene.r, scene.g, scene.b
	local lum = 0.2126 * r + 0.7152 * g + 0.0722 * b
	local bright = math.Clamp((0.05 + 2.2 * lum) * cvLightScale:GetFloat(), 0.08, 1.5)
	-- Half the scene's colour, half white: the tint without going fully blue at night.
	local m = math.max(r, g, b, 0.001)
	local tr, tg, tb = (0.5 + 0.5 * r / m) * bright, (0.5 + 0.5 * g / m) * bright, (0.5 + 0.5 * b / m) * bright
	render.SuppressEngineLighting(true)
	render.ResetModelLighting(tr * 0.55, tg * 0.55, tb * 0.55)
	render.SetModelLighting(BOX_TOP, tr, tg, tb)
	render.SetModelLighting(BOX_FRONT, tr * 0.8, tg * 0.8, tb * 0.8)
	render.SetModelLighting(BOX_BOTTOM, tr * 0.3, tg * 0.3, tb * 0.3)
	litNow = true
end

local function endSceneLight()
	if litNow then
		render.SuppressEngineLighting(false)
		litNow = false
	end
end

hook.Add("PreDrawViewModel", "gmodlight", function()
	if connected and mode == MODE_PLAY then return true end
	beginSceneLight()
end)
hook.Add("PostDrawViewModel", "gmodlight", endSceneLight)
hook.Add("PreDrawPlayerHands", "gmodlight", function()
	if connected and mode == MODE_PLAY then return true end
	beginSceneLight()
end)
hook.Add("PostDrawPlayerHands", "gmodlight", endSceneLight)

-- Dying Light has its own health and stamina; GMod shows its weapon HUD.
local NEVER_HUD = {
	CHudHealth = true, CHudBattery = true, CHudDamageIndicator = true, CHudSuitPower = true,
	CHudPoisonDamageIndicator = true, CHudZoom = true,
}
local HANDS_HUD = {CHudAmmo = true, CHudSecondaryAmmo = true, CHudCrosshair = true, CHudWeaponSelection = true}
hook.Add("HUDShouldDraw", "gmodlight", function(name)
	if not connected then return end
	if NEVER_HUD[name] then return false end
	if HANDS_HUD[name] and mode == MODE_PLAY then return false end
end)

-- GMod's own cursor is an OS cursor, which isn't in the captured frame. Draw one.
-- Points go clockwise on screen, or DrawPoly culls them.
local cursorShape = {{x = 0, y = 0}, {x = 14, y = 14}, {x = 0, y = 20}}
hook.Add("DrawOverlay", "gmodlight", function()
	if not connected or not isMenu(mode) then return end
	-- Not gui.MousePos(): that's the real cursor, which is over Dying Light.
	local mx, my = cursorX, cursorY
	local poly = {}
	for i, p in ipairs(cursorShape) do poly[i] = {x = mx + p.x, y = my + p.y} end
	draw.NoTexture()
	surface.SetDrawColor(255, 255, 255, 255)
	surface.DrawPoly(poly)
	surface.SetDrawColor(0, 0, 0, 255)
	for i, p in ipairs(poly) do
		local q = poly[i % #poly + 1]
		surface.DrawLine(p.x, p.y, q.x, q.y)
	end
end)
