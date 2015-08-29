
#include <cmath>
#include <stdlib.h>
#include <SDL2/SDL_platform.h>
#include <SDL2/SDL_opengl.h>

#include "../core/gl_util.h"
#include "../core/system.h"
#include "../core/console.h"
#include "../core/vmath.h"
#include "../core/polygon.h"
#include "../core/obb.h"
#include "../vt/tr_versions.h"
#include "camera.h"
#include "render.h"
#include "bsp_tree.h"
#include "frustum.h"
#include "shader_description.h"
#include "shader_manager.h"
#include "../world.h"
#include "../script.h"
#include "../mesh.h"
#include "../skeletal_model.h"
#include "../entity.h"
#include "../character_controller.h"
#include "../engine.h"
#include "../engine_physics.h"
#include "../resource.h"

CRender renderer;

void CalculateWaterTint(GLfloat *tint, uint8_t fixed_colour);

#define DEBUG_DRAWER_DEFAULT_BUFFER_SIZE        (128 * 1024)

/*
 * =============================================================================
 */

CRender::CRender():
r_flags(0x00),
r_list(NULL),
r_list_size(0),
r_list_active_count(0),
m_world(NULL),
m_camera(NULL),
m_active_transparency(0),
m_active_texture(0),
frustumManager(NULL),
debugDrawer(NULL),
dynamicBSP(NULL),
shaderManager(NULL)
{
    InitSettings();
    frustumManager = new CFrustumManager(32768);
    debugDrawer    = new CRenderDebugDrawer();
    dynamicBSP     = new CDynamicBSP(512 * 1024);
}

CRender::~CRender()
{
    m_world = NULL;
    m_camera = NULL;

    if(r_list)
    {
        r_list_active_count = 0;
        r_list_size = 0;
        free(r_list);
        r_list = NULL;
    }

    if(frustumManager)
    {
        delete frustumManager;
        frustumManager = NULL;
    }

    if(debugDrawer)
    {
        delete debugDrawer;
        debugDrawer = NULL;
    }

    if(dynamicBSP)
    {
        delete dynamicBSP;
        dynamicBSP = NULL;
    }

    if(shaderManager)
    {
        delete shaderManager;
        shaderManager = NULL;
    }
}

void CRender::InitSettings()
{
    settings.anisotropy = 0;
    settings.lod_bias = 0;
    settings.antialias = 0;
    settings.antialias_samples = 0;
    settings.mipmaps = 3;
    settings.mipmap_mode = 3;
    settings.texture_border = 8;
    settings.z_depth = 16;
    settings.fog_enabled = 1;
    settings.fog_color[0] = 0.0f;
    settings.fog_color[1] = 0.0f;
    settings.fog_color[2] = 0.0f;
    settings.fog_start_depth = 10000.0f;
    settings.fog_end_depth = 16000.0f;
}

void CRender::DoShaders()
{
    if(shaderManager == NULL)
    {
        shaderManager = new shader_manager();
    }
}

void CRender::SetWorld(struct world_s *world)
{
    this->CleanList();
    m_world = NULL;
    r_flags = 0x00;

    if(world)
    {
        uint32_t list_size = world->room_count + 128;                           // magick 128 was added for debug and testing
        if(r_list)
        {
            free(r_list);
        }
        r_list = (struct render_list_s*)malloc(list_size * sizeof(struct render_list_s));
        for(uint32_t i=0; i < list_size; i++)
        {
            r_list[i].active = 0;
            r_list[i].room = NULL;
            r_list[i].dist = 0.0;
        }

        m_world = world;
        r_list_size = list_size;
        r_list_active_count = 0;

        for(uint32_t i=0; i < m_world->room_count; i++)
        {
            m_world->rooms[i].is_in_r_list = 0;
        }
    }
}

// This function is used for updating global animated texture frame
void CRender::UpdateAnimTextures()
{
    if(m_world)
    {
        anim_seq_p seq = m_world->anim_sequences;
        for(uint16_t i=0;i<m_world->anim_sequences_count;i++,seq++)
        {
            if(seq->frame_lock)
            {
                continue;
            }

            seq->frame_time += engine_frame_time;
            if(seq->uvrotate)
            {
                int j = (seq->frame_time / seq->frame_rate);
                seq->frame_time -= (float)j * seq->frame_rate;
                seq->frames[seq->current_frame].current_uvrotate = seq->frame_time * seq->frames[seq->current_frame].uvrotate_max / seq->frame_rate;
            }
            else if(seq->frame_time >= seq->frame_rate)
            {
                int j = (seq->frame_time / seq->frame_rate);
                seq->frame_time -= (float)j * seq->frame_rate;

                switch(seq->anim_type)
                {
                    case TR_ANIMTEXTURE_REVERSE:
                        if(seq->reverse_direction)
                        {
                            if(seq->current_frame == 0)
                            {
                                seq->current_frame++;
                                seq->reverse_direction = false;
                            }
                            else if(seq->current_frame > 0)
                            {
                                seq->current_frame--;
                            }
                        }
                        else
                        {
                            if(seq->current_frame == seq->frames_count - 1)
                            {
                                seq->current_frame--;
                                seq->reverse_direction = true;
                            }
                            else if(seq->current_frame < seq->frames_count - 1)
                            {
                                seq->current_frame++;
                            }
                            seq->current_frame %= seq->frames_count;            ///@PARANOID
                        }
                        break;

                    case TR_ANIMTEXTURE_FORWARD:                                // inversed in polygon anim. texture frames
                    case TR_ANIMTEXTURE_BACKWARD:
                        seq->current_frame++;
                        seq->current_frame %= seq->frames_count;
                        break;
                };
            }
        }
    }
}

/**
 * Renderer list generation by current world and camera
 */
void CRender::GenWorldList(struct camera_s *cam)
{
    if(m_world == NULL)
    {
        return;
    }

    this->CleanList();                                                          // clear old render list
    this->dynamicBSP->Reset(m_world->anim_sequences);
    this->frustumManager->Reset();
    cam->frustum->next = NULL;
    m_camera = cam;

    room_p curr_room = Room_FindPosCogerrence(cam->pos, cam->current_room);     // find room that contains camera

    cam->current_room = curr_room;                                              // set camera's cuttent room pointer
    if(curr_room != NULL)                                                       // camera located in some room
    {
        curr_room->frustum = NULL;                                              // room with camera inside has no frustums!
        curr_room->max_path = 0;
        this->AddRoom(curr_room);                                               // room with camera inside adds to the render list immediately
        portal_p p = curr_room->portals;                                        // pointer to the portals array
        for(uint16_t i=0; i<curr_room->portal_count; i++,p++)                   // go through all start room portals
        {
            frustum_p last_frus = this->frustumManager->PortalFrustumIntersect(p, cam->frustum, cam);
            if(last_frus)
            {
                this->AddRoom(p->dest_room);                                    // portal destination room
                last_frus->parents_count = 1;                                   // created by camera
                this->ProcessRoom(p, last_frus);                                // next start reccursion algorithm
            }
        }
    }
    else                                                                        // camera is out of all rooms
    {
        curr_room = m_world->rooms;                                             // draw full level. Yes - it is slow, but it is not gameplay - it is debug.
        for(uint32_t i=0; i<m_world->room_count; i++,curr_room++)
        {
            if(Frustum_IsAABBVisible(curr_room->bb_min, curr_room->bb_max, cam->frustum))
            {
                this->AddRoom(curr_room);
            }
        }
    }
}

/**
 * Render all visible rooms
 */
void CRender::DrawList()
{
    if(!m_world)
    {
        return;
    }

    if(r_flags & R_DRAW_WIRE)
    {
        glPolygonMode(GL_FRONT, GL_LINE);
    }
    else if(r_flags & R_DRAW_POINTS)
    {
        glEnable(GL_POINT_SMOOTH);
        glPointSize(4);
        glPolygonMode(GL_FRONT, GL_POINT);
    }
    else
    {
        glPolygonMode(GL_FRONT, GL_FILL);
    }

    glEnable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glEnable(GL_ALPHA_TEST);

    m_active_texture = 0;
    this->DrawSkyBox(m_camera->gl_view_proj_mat);

    if(m_world->Character)
    {
        this->DrawEntity(m_world->Character, m_camera->gl_view_mat, m_camera->gl_view_proj_mat);
    }

    /*
     * room rendering
     */
    for(uint32_t i=0; i<r_list_active_count; i++)
    {
        this->DrawRoom(r_list[i].room, m_camera->gl_view_mat, m_camera->gl_view_proj_mat);
    }

    glDisable(GL_CULL_FACE);
    glDisableClientState(GL_NORMAL_ARRAY);                                      ///@FIXME: reduce number of gl state changes
    for(uint32_t i=0; i<r_list_active_count; i++)
    {
        this->DrawRoomSprites(r_list[i].room, m_camera->gl_view_mat, m_camera->gl_proj_mat);
    }
    glEnableClientState(GL_NORMAL_ARRAY);

    /*
     * NOW render transparency polygons
     */
    /*First generate BSP from base room mesh - it has good for start splitter polygons*/
    for(uint32_t i=0;i<r_list_active_count;i++)
    {
        room_p r = r_list[i].room;
        if((r->mesh != NULL) && (r->mesh->transparency_polygons != NULL))
        {
            dynamicBSP->AddNewPolygonList(r->mesh->transparency_polygons, r->transform, m_camera->frustum);
        }
    }

    for(uint32_t i=0;i<r_list_active_count;i++)
    {
        room_p r = r_list[i].room;
        // Add transparency polygons from static meshes (if they exists)
        for(uint16_t j=0;j<r->static_mesh_count;j++)
        {
            if((r->static_mesh[j].mesh->transparency_polygons != NULL) && Frustum_IsOBBVisibleInFrustumList(r->static_mesh[j].obb, (r->frustum)?(r->frustum):(m_camera->frustum)))
            {
                dynamicBSP->AddNewPolygonList(r->static_mesh[j].mesh->transparency_polygons, r->static_mesh[j].transform, m_camera->frustum);
            }
        }

        // Add transparency polygons from all entities (if they exists) // yes, entities may be animated and intersects with each others;
        for(engine_container_p cont=r->containers;cont!=NULL;cont=cont->next)
        {
            if(cont->object_type == OBJECT_ENTITY)
            {
                entity_p ent = (entity_p)cont->object;
                if((ent->bf->animations.model->transparency_flags == MESH_HAS_TRANSPARENCY) && (ent->state_flags & ENTITY_STATE_VISIBLE) && Frustum_IsOBBVisibleInFrustumList(ent->obb, (r->frustum)?(r->frustum):(m_camera->frustum)))
                {
                    float tr[16];
                    for(uint16_t j=0;j<ent->bf->bone_tag_count;j++)
                    {
                        if(ent->bf->bone_tags[j].mesh_base->transparency_polygons != NULL)
                        {
                            Mat4_Mat4_mul(tr, ent->transform, ent->bf->bone_tags[j].full_transform);
                            dynamicBSP->AddNewPolygonList(ent->bf->bone_tags[j].mesh_base->transparency_polygons, tr, m_camera->frustum);
                        }
                    }
                }
            }
        }
    }

    if((engine_world.Character != NULL) && (engine_world.Character->bf->animations.model->transparency_flags == MESH_HAS_TRANSPARENCY))
    {
        float tr[16];
        entity_p ent = engine_world.Character;
        for(uint16_t j=0;j<ent->bf->bone_tag_count;j++)
        {
            if(ent->bf->bone_tags[j].mesh_base->transparency_polygons != NULL)
            {
                Mat4_Mat4_mul(tr, ent->transform, ent->bf->bone_tags[j].full_transform);
                dynamicBSP->AddNewPolygonList(ent->bf->bone_tags[j].mesh_base->transparency_polygons, tr, m_camera->frustum);
            }
        }
    }

    if((dynamicBSP->m_root->polygons_front != NULL) && (dynamicBSP->m_vbo != 0))
    {
        const unlit_tinted_shader_description *shader = shaderManager->getRoomShader(false, false);
        glUseProgramObjectARB(shader->program);
        glUniform1iARB(shader->sampler, 0);
        glUniformMatrix4fvARB(shader->model_view_projection, 1, false, m_camera->gl_view_proj_mat);
        glDepthMask(GL_FALSE);
        glDisable(GL_ALPHA_TEST);
        glEnable(GL_BLEND);
        m_active_transparency = 0;
        glBindBufferARB(GL_ARRAY_BUFFER_ARB, dynamicBSP->m_vbo);
        glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, 0);
        glBufferDataARB(GL_ARRAY_BUFFER_ARB, dynamicBSP->GetActiveVertexCount() * sizeof(vertex_t), dynamicBSP->GetVertexArray(), GL_DYNAMIC_DRAW);
        glVertexPointer(3, GL_BT_SCALAR, sizeof(vertex_t), (void*)offsetof(vertex_t, position));
        glColorPointer(4, GL_FLOAT, sizeof(vertex_t), (void*)offsetof(vertex_t, color));
        glNormalPointer(GL_FLOAT, sizeof(vertex_t), (void*)offsetof(vertex_t, normal));
        glTexCoordPointer(2, GL_FLOAT, sizeof(vertex_t), (void*)offsetof(vertex_t, tex_coord));
        this->DrawBSPBackToFront(dynamicBSP->m_root);
        glBindBufferARB(GL_ARRAY_BUFFER_ARB, 0);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
    }
    //Reset polygon draw mode
    glPolygonMode(GL_FRONT, GL_FILL);
    m_active_texture = 0;
}

void CRender::DrawListDebugLines()
{
    if (m_world && (r_flags & (R_DRAW_BOXES | R_DRAW_ROOMBOXES | R_DRAW_PORTALS | R_DRAW_FRUSTUMS | R_DRAW_AXIS | R_DRAW_NORMALS | R_DRAW_COLL)))
    {
        debugDrawer->SetDrawFlags(r_flags);

        if(m_world->Character)
        {
            debugDrawer->DrawEntityDebugLines(m_world->Character);
        }

        /*
         * Render world debug information
         */
        if((r_flags & R_DRAW_NORMALS) && (m_world->sky_box != NULL))
        {
            GLfloat tr[16];
            float *p;
            Mat4_E_macro(tr);
            p = m_world->sky_box->animations->frames->bone_tags->offset;
            vec3_add(tr+12, m_camera->pos, p);
            p = m_world->sky_box->animations->frames->bone_tags->qrotate;
            Mat4_set_qrotation(tr, p);
            debugDrawer->DrawMeshDebugLines(m_world->sky_box->mesh_tree->mesh_base, tr, NULL, NULL);
        }

        for(uint32_t i=0; i<r_list_active_count; i++)
        {
            debugDrawer->DrawRoomDebugLines(r_list[i].room, m_camera);
        }

        if(r_flags & R_DRAW_COLL)
        {
            Physics_DebugDrawWorld();
        }
    }

    if(!debugDrawer->IsEmpty())
    {
        const unlit_tinted_shader_description *shader = shaderManager->getRoomShader(false, false);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glUseProgramObjectARB(shader->program);
        glUniform1iARB(shader->sampler, 0);
        glUniformMatrix4fvARB(shader->model_view_projection, 1, false, m_camera->gl_view_proj_mat);
        glBindBufferARB(GL_ARRAY_BUFFER_ARB, 0);
        m_active_texture = 0;
        BindWhiteTexture();
        glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, 0);
        glPointSize( 6.0f );
        glLineWidth( 3.0f );
        debugDrawer->Render();
    }
    debugDrawer->Reset();
}

void CRender::CleanList()
{
    if(m_world && m_world->Character)
    {
        m_world->Character->was_rendered = 0;
        m_world->Character->was_rendered_lines = 0;
    }

    for(uint32_t i=0; i<r_list_active_count; i++)
    {
        r_list[i].active = 0;
        r_list[i].dist = 0.0;
        room_p r = r_list[i].room;
        r_list[i].room = NULL;

        r->is_in_r_list = 0;
        r->active_frustums = 0;
        r->frustum = NULL;
    }

    r_flags &= ~R_DRAW_SKYBOX;
    r_list_active_count = 0;
}

/*
 * Draw objects functions
 */
void CRender::DrawBSPPolygon(struct bsp_polygon_s *p)
{
    // Blending mode switcher.
    // Note that modes above 2 aren't explicitly used in TR textures, only for
    // internal particle processing. Theoretically it's still possible to use
    // them if you will force type via TRTextur utility.
    if(m_active_transparency != p->transparency)
    {
        m_active_transparency = p->transparency;
        switch(m_active_transparency)
        {
            case BM_MULTIPLY:                                    // Classic PC alpha
                glBlendFunc(GL_ONE, GL_ONE);
                break;

            case BM_INVERT_SRC:                                  // Inversion by src (PS darkness) - SAME AS IN TR3-TR5
                glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_COLOR);
                break;

            case BM_INVERT_DEST:                                 // Inversion by dest
                glBlendFunc(GL_ONE_MINUS_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR);
                break;

            case BM_SCREEN:                                      // Screen (smoke, etc.)
                glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR);
                break;

            case BM_ANIMATED_TEX:
                glBlendFunc(GL_ONE, GL_ZERO);
                break;

            default:                                             // opaque animated textures case
                break;
        };
    }

    if(m_active_texture != m_world->textures[p->tex_index])
    {
        m_active_texture = m_world->textures[p->tex_index];
        glBindTexture(GL_TEXTURE_2D, m_active_texture);
    }
    glDrawElements(GL_TRIANGLE_FAN, p->vertex_count, GL_UNSIGNED_INT, p->indexes);
}

void CRender::DrawBSPFrontToBack(struct bsp_node_s *root)
{
    float d = vec3_plane_dist(root->plane, engine_camera.pos);

    if(d >= 0)
    {
        if(root->front != NULL)
        {
            this->DrawBSPFrontToBack(root->front);
        }

        for(bsp_polygon_p p=root->polygons_front;p!=NULL;p=p->next)
        {
            this->DrawBSPPolygon(p);
        }
        for(bsp_polygon_p p=root->polygons_back;p!=NULL;p=p->next)
        {
            this->DrawBSPPolygon(p);
        }

        if(root->back != NULL)
        {
            this->DrawBSPFrontToBack(root->back);
        }
    }
    else
    {
        if(root->back != NULL)
        {
            this->DrawBSPFrontToBack(root->back);
        }

        for(bsp_polygon_p p=root->polygons_back;p!=NULL;p=p->next)
        {
            this->DrawBSPPolygon(p);
        }
        for(bsp_polygon_p p=root->polygons_front;p!=NULL;p=p->next)
        {
            this->DrawBSPPolygon(p);
        }

        if(root->front != NULL)
        {
            this->DrawBSPFrontToBack(root->front);
        }
    }
}

void CRender::DrawBSPBackToFront(struct bsp_node_s *root)
{
    float d = vec3_plane_dist(root->plane, engine_camera.pos);

    if(d >= 0)
    {
        if(root->back != NULL)
        {
            this->DrawBSPBackToFront(root->back);
        }

        for(bsp_polygon_p p=root->polygons_back;p!=NULL;p=p->next)
        {
            this->DrawBSPPolygon(p);
        }
        for(bsp_polygon_p p=root->polygons_front;p!=NULL;p=p->next)
        {
            this->DrawBSPPolygon(p);
        }

        if(root->front != NULL)
        {
            this->DrawBSPBackToFront(root->front);
        }
    }
    else
    {
        if(root->front != NULL)
        {
            this->DrawBSPBackToFront(root->front);
        }

        for(bsp_polygon_p p=root->polygons_front;p!=NULL;p=p->next)
        {
            this->DrawBSPPolygon(p);
        }
        for(bsp_polygon_p p=root->polygons_back;p!=NULL;p=p->next)
        {
            this->DrawBSPPolygon(p);
        }

        if(root->back != NULL)
        {
            this->DrawBSPBackToFront(root->back);
        }
    }
}

void CRender::DrawMesh(struct base_mesh_s *mesh, const float *overrideVertices, const float *overrideNormals)
{
    if(mesh->num_animated_elements > 0)
    {
        // Respecify the tex coord buffer
        glBindBufferARB(GL_ARRAY_BUFFER, mesh->animated_texcoord_array);
        // Tell OpenGL to discard the old values
        glBufferDataARB(GL_ARRAY_BUFFER, mesh->num_animated_elements * sizeof(GLfloat [2]), 0, GL_STREAM_DRAW);
        // Get writable data (to avoid copy)
        GLfloat *data = (GLfloat *) glMapBufferARB(GL_ARRAY_BUFFER, GL_WRITE_ONLY);

        for(polygon_p p=mesh->animated_polygons;p!=NULL;p=p->next)
        {
            anim_seq_p seq = engine_world.anim_sequences + p->anim_id - 1;
            uint16_t frame = (seq->current_frame + p->frame_offset) % seq->frames_count;
            tex_frame_p tf = seq->frames + frame;
            for(uint16_t i=0;i<p->vertex_count;i++,data+=2)
            {
                ApplyAnimTextureTransformation(data, p->vertices[i].tex_coord, tf);
            }
        }
        glUnmapBufferARB(GL_ARRAY_BUFFER);

        // Setup altered buffer
        glTexCoordPointer(2, GL_FLOAT, sizeof(GLfloat [2]), 0);
        // Setup static data
        glBindBufferARB(GL_ARRAY_BUFFER, mesh->animated_vertex_array);
        glVertexPointer(3, GL_BT_SCALAR, sizeof(GLfloat [10]), 0);
        glColorPointer(4, GL_FLOAT, sizeof(GLfloat [10]), (void *) sizeof(GLfloat [3]));
        glNormalPointer(GL_FLOAT, sizeof(GLfloat [10]), (void *) sizeof(GLfloat [7]));

        glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER, mesh->animated_index_array);
        if(m_active_texture != m_world->textures[0])                              ///@FIXME: UGLY HACK!
        {
            m_active_texture = m_world->textures[0];
            glBindTexture(GL_TEXTURE_2D, m_active_texture);
        }
        glDrawElements(GL_TRIANGLES, mesh->animated_index_array_length, GL_UNSIGNED_INT, 0);
    }

    if(mesh->vertex_count == 0)
    {
        return;
    }

    if(mesh->vbo_vertex_array)
    {
        glBindBufferARB(GL_ARRAY_BUFFER_ARB, mesh->vbo_vertex_array);
        glVertexPointer(3, GL_BT_SCALAR, sizeof(vertex_t), (void*)offsetof(vertex_t, position));
        glColorPointer(4, GL_FLOAT, sizeof(vertex_t), (void*)offsetof(vertex_t, color));
        glNormalPointer(GL_FLOAT, sizeof(vertex_t), (void*)offsetof(vertex_t, normal));
        glTexCoordPointer(2, GL_FLOAT, sizeof(vertex_t), (void*)offsetof(vertex_t, tex_coord));
    }

    // Bind overriden vertices if they exist
    if (overrideVertices != NULL)
    {
        // Standard normals are always float. Overridden normals (from skinning)
        // are float.
        glBindBufferARB(GL_ARRAY_BUFFER_ARB, 0);
        glVertexPointer(3, GL_BT_SCALAR, 0, overrideVertices);
        glNormalPointer(GL_BT_SCALAR, 0, overrideNormals);
    }

    const uint32_t *elementsbase = mesh->elements;
        glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, mesh->vbo_index_array);
        elementsbase = NULL;

    unsigned long offset = 0;
    for(uint32_t texture = 0; texture < mesh->num_texture_pages; texture++)
    {
        if(mesh->element_count_per_texture[texture] == 0)
        {
            continue;
        }

        if(m_active_texture != m_world->textures[texture])
        {
            m_active_texture = m_world->textures[texture];
            glBindTexture(GL_TEXTURE_2D, m_active_texture);
        }
        glDrawElements(GL_TRIANGLES, mesh->element_count_per_texture[texture], GL_UNSIGNED_INT, elementsbase + offset);
        offset += mesh->element_count_per_texture[texture];
    }
}

void CRender::DrawSkinMesh(struct base_mesh_s *mesh, float transform[16])
{
    uint32_t i;
    vertex_p v;
    float *p_vertex, *src_v, *dst_v, t;
    GLfloat *p_normale, *src_n, *dst_n;
    int8_t *ch = mesh->skin_map;
    size_t buf_size = mesh->vertex_count * 3 * sizeof(GLfloat);

    p_vertex  = (GLfloat*)Sys_GetTempMem(buf_size);
    p_normale = (GLfloat*)Sys_GetTempMem(buf_size);
    dst_v = p_vertex;
    dst_n = p_normale;
    v = mesh->vertices;
    for(i=0; i<mesh->vertex_count; i++,v++)
    {
        src_v = v->position;
        src_n = v->normal;
        switch(*ch)
        {
        case 0:
            dst_v[0]  = transform[0] * src_v[0] + transform[1] * src_v[1] + transform[2]  * src_v[2];             // (M^-1 * src).x
            dst_v[1]  = transform[4] * src_v[0] + transform[5] * src_v[1] + transform[6]  * src_v[2];             // (M^-1 * src).y
            dst_v[2]  = transform[8] * src_v[0] + transform[9] * src_v[1] + transform[10] * src_v[2];             // (M^-1 * src).z

            dst_n[0]  = transform[0] * src_n[0] + transform[1] * src_n[1] + transform[2]  * src_n[2];             // (M^-1 * src).x
            dst_n[1]  = transform[4] * src_n[0] + transform[5] * src_n[1] + transform[6]  * src_n[2];             // (M^-1 * src).y
            dst_n[2]  = transform[8] * src_n[0] + transform[9] * src_n[1] + transform[10] * src_n[2];             // (M^-1 * src).z

            vec3_add(dst_v, dst_v, src_v);
            dst_v[0] /= 2.0;
            dst_v[1] /= 2.0;
            dst_v[2] /= 2.0;
            vec3_add(dst_n, dst_n, src_n);
            vec3_norm(dst_n, t);
            break;

        case 2:
            dst_v[0]  = transform[0] * src_v[0] + transform[1] * src_v[1] + transform[2]  * src_v[2];             // (M^-1 * src).x
            dst_v[1]  = transform[4] * src_v[0] + transform[5] * src_v[1] + transform[6]  * src_v[2];             // (M^-1 * src).y
            dst_v[2]  = transform[8] * src_v[0] + transform[9] * src_v[1] + transform[10] * src_v[2];             // (M^-1 * src).z

            dst_n[0]  = transform[0] * src_n[0] + transform[1] * src_n[1] + transform[2]  * src_n[2];             // (M^-1 * src).x
            dst_n[1]  = transform[4] * src_n[0] + transform[5] * src_n[1] + transform[6]  * src_n[2];             // (M^-1 * src).y
            dst_n[2]  = transform[8] * src_n[0] + transform[9] * src_n[1] + transform[10] * src_n[2];             // (M^-1 * src).z
            //vec3_copy(dst_n, src_n);
            break;

        case 1:
            vec3_copy(dst_v, src_v);
            vec3_copy(dst_n, src_n);
            break;
        }
        ch++;
        dst_v += 3;
        dst_n += 3;
    }

    this->DrawMesh(mesh, p_vertex, p_normale);
    Sys_ReturnTempMem(2 * buf_size);
}

void CRender::DrawSkyBox(const float modelViewProjectionMatrix[16])
{
    float tr[16];
    float *p;

    if((r_flags & R_DRAW_SKYBOX) && (m_world != NULL) && (m_world->sky_box != NULL))
    {
        glDepthMask(GL_FALSE);
        tr[15] = 1.0;
        p = m_world->sky_box->animations->frames->bone_tags->offset;
        vec3_add(tr+12, m_camera->pos, p);
        p = m_world->sky_box->animations->frames->bone_tags->qrotate;
        Mat4_set_qrotation(tr, p);
        float fullView[16];
        Mat4_Mat4_mul(fullView, modelViewProjectionMatrix, tr);

        const unlit_tinted_shader_description *shader = shaderManager->getStaticMeshShader();
        glUseProgramObjectARB(shader->program);
        glUniformMatrix4fvARB(shader->model_view_projection, 1, false, fullView);
        glUniform1iARB(shader->sampler, 0);
        GLfloat tint[] = { 1, 1, 1, 1 };
        glUniform4fvARB(shader->tint_mult, 1, tint);

        this->DrawMesh(m_world->sky_box->mesh_tree->mesh_base, NULL, NULL);
        glDepthMask(GL_TRUE);
    }
}

/**
 * skeletal model drawing
 */
void CRender::DrawSkeletalModel(const lit_shader_description *shader, struct ss_bone_frame_s *bframe, const float mvMatrix[16], const float mvpMatrix[16])
{
    ss_bone_tag_p btag = bframe->bone_tags;

    //mvMatrix = modelViewMatrix x entity->transform
    //mvpMatrix = modelViewProjectionMatrix x entity->transform

    for(uint16_t i=0; i<bframe->bone_tag_count; i++,btag++)
    {
        float mvTransform[16];
        Mat4_Mat4_mul(mvTransform, mvMatrix, btag->full_transform);
        glUniformMatrix4fvARB(shader->model_view, 1, false, mvTransform);

        float mvpTransform[16];
        Mat4_Mat4_mul(mvpTransform, mvpMatrix, btag->full_transform);
        glUniformMatrix4fvARB(shader->model_view_projection, 1, false, mvpTransform);

        this->DrawMesh(btag->mesh_base, NULL, NULL);
        if(btag->mesh_slot)
        {
            this->DrawMesh(btag->mesh_slot, NULL, NULL);
        }
        if(btag->mesh_skin)
        {
            this->DrawSkinMesh(btag->mesh_skin, btag->transform);
        }
    }
}

void CRender::DrawEntity(struct entity_s *entity, const float modelViewMatrix[16], const float modelViewProjectionMatrix[16])
{
    if(entity->was_rendered || !(entity->state_flags & ENTITY_STATE_VISIBLE) || (entity->bf->animations.model->hide && !(r_flags & R_DRAW_NULLMESHES)))
    {
        return;
    }

    // Calculate lighting
    const lit_shader_description *shader = this->SetupEntityLight(entity, modelViewMatrix);

    if(entity->bf->animations.model && entity->bf->animations.model->animations)
    {
        float subModelView[16];
        float subModelViewProjection[16];
        if(entity->bf->bone_tag_count == 1)
        {
            float scaledTransform[16];
            memcpy(scaledTransform, entity->transform, sizeof(float) * 16);
            Mat4_Scale(scaledTransform, entity->scaling[0], entity->scaling[1], entity->scaling[2]);
            Mat4_Mat4_mul(subModelView, modelViewMatrix, scaledTransform);
            Mat4_Mat4_mul(subModelViewProjection, modelViewProjectionMatrix, scaledTransform);
        }
        else
        {
            Mat4_Mat4_mul(subModelView, modelViewMatrix, entity->transform);
            Mat4_Mat4_mul(subModelViewProjection, modelViewProjectionMatrix, entity->transform);
        }

        this->DrawSkeletalModel(shader, entity->bf, subModelView, subModelViewProjection);

        if(entity->character && entity->character->hair_count)
        {
            base_mesh_p mesh;
            float transform[16];
            for(int h=0; h<entity->character->hair_count; h++)
            {
                int num_elements = Hair_GetElementsCount(entity->character->hairs[h]);
                for(uint16_t i=0; i<num_elements; i++)
                {
                    Hair_GetElementInfo(entity->character->hairs[h], i, &mesh, transform);
                    Mat4_Mat4_mul(subModelView, modelViewMatrix, transform);
                    Mat4_Mat4_mul(subModelViewProjection, modelViewProjectionMatrix, transform);

                    glUniformMatrix4fvARB(shader->model_view, 1, GL_FALSE, subModelView);
                    glUniformMatrix4fvARB(shader->model_view_projection, 1, GL_FALSE, subModelViewProjection);
                    this->DrawMesh(mesh, NULL, NULL);
                }
            }
        }
    }
}

void CRender::DrawRoom(struct room_s *room, const float modelViewMatrix[16], const float modelViewProjectionMatrix[16])
{
    engine_container_p cont;
    entity_p ent;

    const shader_description *lastShader = 0;

#if STENCIL_FRUSTUM
    ////start test stencil test code
    bool need_stencil = false;
    if(room->frustum != NULL)
    {
        for(uint16_t i=0;i<room->overlapped_room_list_size;i++)
        {
            if(room->overlapped_room_list[i]->is_in_r_list)
            {
                need_stencil = true;
                break;
            }
        }

        if(need_stencil)
        {
            const int elem_size = (3 + 3 + 4 + 2) * sizeof(GLfloat);
            const unlit_tinted_shader_description *shader = shaderManager->getRoomShader(false, false);
            size_t buf_size;

            glUseProgramObjectARB(shader->program);
            glUniform1iARB(shader->sampler, 0);
            glUniformMatrix4fvARB(shader->model_view_projection, 1, false, engine_camera.gl_view_proj_mat);
            glEnable(GL_STENCIL_TEST);
            glClear(GL_STENCIL_BUFFER_BIT);
            glStencilFunc(GL_NEVER, 1, 0x00);
            glStencilOp(GL_REPLACE, GL_KEEP, GL_KEEP);
            for(frustum_p f=room->frustum;f!=NULL;f=f->next)
            {
                buf_size = f->vertex_count * elem_size;
                GLfloat *v, *buf = (GLfloat*)Sys_GetTempMem(buf_size);
                v=buf;
                for(int16_t i=f->vertex_count-1;i>=0;i--)
                {
                    vec3_copy(v, f->vertex+3*i);                    v+=3;
                    vec3_copy_inv(v, engine_camera.view_dir);       v+=3;
                    vec4_set_one(v);                                v+=4;
                    v[0] = v[1] = 0.0;                              v+=2;
                }

                m_active_texture = 0;
                BindWhiteTexture();
                glBindBufferARB(GL_ARRAY_BUFFER_ARB, 0);
                glVertexPointer(3, GL_FLOAT, elem_size, buf+0);
                glNormalPointer(GL_FLOAT, elem_size, buf+3);
                glColorPointer(4, GL_FLOAT, elem_size, buf+3+3);
                glTexCoordPointer(2, GL_FLOAT, elem_size, buf+3+3+4);
                glDrawArrays(GL_TRIANGLE_FAN, 0, f->vertex_count);

                Sys_ReturnTempMem(buf_size);
            }
            glStencilFunc(GL_EQUAL, 1, 0xFF);
        }
    }
#endif

    if(!(r_flags & R_SKIP_ROOM) && room->mesh)
    {
        float modelViewProjectionTransform[16];
        Mat4_Mat4_mul(modelViewProjectionTransform, modelViewProjectionMatrix, room->transform);

        const unlit_tinted_shader_description *shader = shaderManager->getRoomShader(room->light_mode == 1, room->flags & 1);

        GLfloat tint[4];
        CalculateWaterTint(tint, 1);
        if (shader != lastShader)
        {
            glUseProgramObjectARB(shader->program);
        }

        lastShader = shader;
        glUniform4fvARB(shader->tint_mult, 1, tint);
        glUniform1fARB(shader->current_tick, (GLfloat) SDL_GetTicks());
        glUniform1iARB(shader->sampler, 0);
        glUniformMatrix4fvARB(shader->model_view_projection, 1, false, modelViewProjectionTransform);
        this->DrawMesh(room->mesh, NULL, NULL);
    }

    if (room->static_mesh_count > 0)
    {
        glUseProgramObjectARB(shaderManager->getStaticMeshShader()->program);
        for(uint32_t i=0; i<room->static_mesh_count; i++)
        {
            if(room->static_mesh[i].was_rendered || !Frustum_IsOBBVisibleInFrustumList(room->static_mesh[i].obb, (room->frustum)?(room->frustum):(m_camera->frustum)))
            {
                continue;
            }

            if((room->static_mesh[i].hide == 1) && !(r_flags & R_DRAW_DUMMY_STATICS))
            {
                continue;
            }

            float transform[16];
            Mat4_Mat4_mul(transform, modelViewProjectionMatrix, room->static_mesh[i].transform);
            glUniformMatrix4fvARB(shaderManager->getStaticMeshShader()->model_view_projection, 1, false, transform);
            base_mesh_s *mesh = room->static_mesh[i].mesh;
            GLfloat tint[4];

            vec4_copy(tint, room->static_mesh[i].tint);

            //If this static mesh is in a water room
            if(room->flags & TR_ROOM_FLAG_WATER)
            {
                CalculateWaterTint(tint, 0);
            }
            glUniform4fvARB(shaderManager->getStaticMeshShader()->tint_mult, 1, tint);
            this->DrawMesh(mesh, NULL, NULL);
            room->static_mesh[i].was_rendered = 1;
        }
    }

    if (room->containers)
    {
        for(cont=room->containers; cont; cont=cont->next)
        {
            switch(cont->object_type)
            {
            case OBJECT_ENTITY:
                ent = (entity_p)cont->object;
                if(ent->was_rendered == 0)
                {
                    if(Frustum_IsOBBVisibleInFrustumList(ent->obb, (room->frustum)?(room->frustum):(m_camera->frustum)))
                    {
                        this->DrawEntity(ent, modelViewMatrix, modelViewProjectionMatrix);
                    }
                    ent->was_rendered = 1;
                }
                break;
            };
        }
    }
#if STENCIL_FRUSTUM
    if(need_stencil)
    {
        glDisable(GL_STENCIL_TEST);
    }
#endif
}

void CRender::DrawRoomSprites(struct room_s *room, const float modelViewMatrix[16], const float projectionMatrix[16])
{
    if (room->sprites_count > 0 && room->sprite_buffer)
    {
        const sprite_shader_description *shader = shaderManager->getSpriteShader();
        glUseProgramObjectARB(shader->program);
        glUniformMatrix4fvARB(shader->model_view, 1, GL_FALSE, modelViewMatrix);
        glUniformMatrix4fvARB(shader->projection, 1, GL_FALSE, projectionMatrix);
        glUniform1iARB(shader->sampler, 0);

        glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);
        glDisableClientState(GL_VERTEX_ARRAY);
        glDisableClientState(GL_NORMAL_ARRAY);
        glDisableClientState(GL_COLOR_ARRAY);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);

        glBindBufferARB(GL_ARRAY_BUFFER_ARB, room->sprite_buffer->array_buffer);

        glEnableVertexAttribArrayARB(sprite_shader_description::vertex_attribs::position);
        glVertexAttribPointerARB(sprite_shader_description::vertex_attribs::position, 3, GL_FLOAT, GL_FALSE, sizeof(GLfloat [7]), (const GLvoid *) sizeof(GLfloat [0]));

        glEnableVertexAttribArrayARB(sprite_shader_description::vertex_attribs::tex_coord);
        glVertexAttribPointerARB(sprite_shader_description::vertex_attribs::tex_coord, 2, GL_FLOAT, GL_FALSE, sizeof(GLfloat [7]), (const GLvoid *) sizeof(GLfloat [3]));

        glEnableVertexAttribArrayARB(sprite_shader_description::vertex_attribs::corner_offset);
        glVertexAttribPointerARB(sprite_shader_description::vertex_attribs::corner_offset, 2, GL_FLOAT, GL_FALSE, sizeof(GLfloat [7]), (const GLvoid *) sizeof(GLfloat [5]));

        glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, room->sprite_buffer->element_array_buffer);

        unsigned long offset = 0;
        for(uint32_t texture = 0; texture < room->sprite_buffer->num_texture_pages; texture++)
        {
            if(room->sprite_buffer->element_count_per_texture[texture] == 0)
            {
                continue;
            }

            if(m_active_texture != m_world->textures[texture])
            {
                m_active_texture = m_world->textures[texture];
                glBindTexture(GL_TEXTURE_2D, m_active_texture);
            }
            glDrawElements(GL_TRIANGLES, room->sprite_buffer->element_count_per_texture[texture], GL_UNSIGNED_SHORT, (GLvoid *) (offset * sizeof(uint16_t)));
            offset += room->sprite_buffer->element_count_per_texture[texture];
        }

        glDisableVertexAttribArrayARB(sprite_shader_description::vertex_attribs::position);
        glDisableVertexAttribArrayARB(sprite_shader_description::vertex_attribs::tex_coord);
        glDisableVertexAttribArrayARB(sprite_shader_description::vertex_attribs::corner_offset);
        glPopClientAttrib();
    }
}

int  CRender::AddRoom(struct room_s *room)
{
    int ret = 0;
    engine_container_p cont;
    float dist, centre[3];

    if(room->is_in_r_list || !room->active)
    {
        return 0;
    }

    centre[0] = (room->bb_min[0] + room->bb_max[0]) / 2;
    centre[1] = (room->bb_min[1] + room->bb_max[1]) / 2;
    centre[2] = (room->bb_min[2] + room->bb_max[2]) / 2;
    dist = vec3_dist(m_camera->pos, centre);

    if(r_list_active_count < r_list_size)
    {
        r_list[r_list_active_count].room = room;
        r_list[r_list_active_count].active = 1;
        r_list[r_list_active_count].dist = dist;
        r_list_active_count++;
        ret++;

        if(room->flags & TR_ROOM_FLAG_SKYBOX)
        {
            r_flags |= R_DRAW_SKYBOX;
        }
    }

    for(uint32_t i=0; i<room->static_mesh_count; i++)
    {
        room->static_mesh[i].was_rendered = 0;
        room->static_mesh[i].was_rendered_lines = 0;
    }

    for(cont=room->containers; cont; cont=cont->next)
    {
        switch(cont->object_type)
        {
            case OBJECT_ENTITY:
                ((entity_p)cont->object)->was_rendered = 0;
                ((entity_p)cont->object)->was_rendered_lines = 0;
                break;
        };
    }

    for(uint32_t i=0; i<room->sprites_count; i++)
    {
        room->sprites[i].was_rendered = 0;
    }

    room->is_in_r_list = 1;

    return ret;
}

/**
 * The reccursion algorithm: go through the rooms with portal - frustum occlusion test
 * @portal - we entered to the room through that portal
 * @frus - frustum that intersects the portal
 * @return number of added rooms
 */
int CRender::ProcessRoom(struct portal_s *portal, struct frustum_s *frus)
{
    int ret = 0;
    room_p room = portal->dest_room;                                            // куда ведет портал
    room_p src_room = portal->current_room;                                     // откуда ведет портал

    if((src_room == NULL) || !src_room->active || (room == NULL) || !room->active)
    {
        return 0;
    }

    for(uint16_t i=0; i<room->portal_count; i++)                                // перебираем все порталы входной комнаты
    {
        portal_p p = room->portals + i;
        if((p->dest_room->active) && (p->dest_room != src_room))                // обратно идти даже не пытаемся
        {
            frustum_p gen_frus = frustumManager->PortalFrustumIntersect(p, frus, m_camera);// Главная ф-я портального рендерера. Тут и проверка
            if(gen_frus)                                                        // на пересечение и генерация фрустума по порталу
            {
                ret++;
                this->AddRoom(p->dest_room);
                this->ProcessRoom(p, gen_frus);
            }
        }
    }
    return ret;
}

/**
 * Sets up the light calculations for the given entity based on its current
 * room. Returns the used shader, which will have been made current already.
 */
const lit_shader_description *CRender::SetupEntityLight(struct entity_s *entity, const float modelViewMatrix[16])
{
    // Calculate lighting
    const lit_shader_description *shader;

    room_s *room = entity->self->room;
    if(room != NULL)
    {
        GLfloat ambient_component[4];

        ambient_component[0] = room->ambient_lighting[0];
        ambient_component[1] = room->ambient_lighting[1];
        ambient_component[2] = room->ambient_lighting[2];
        ambient_component[3] = 1.0f;

        if(room->flags & TR_ROOM_FLAG_WATER)
        {
            CalculateWaterTint(ambient_component, 0);
        }

        GLenum current_light_number = 0;
        light_s *current_light = NULL;

        GLfloat positions[3*MAX_NUM_LIGHTS];
        GLfloat colors[4*MAX_NUM_LIGHTS];
        GLfloat innerRadiuses[1*MAX_NUM_LIGHTS];
        GLfloat outerRadiuses[1*MAX_NUM_LIGHTS];
        memset(positions, 0, sizeof(positions));
        memset(colors, 0, sizeof(colors));
        memset(innerRadiuses, 0, sizeof(innerRadiuses));
        memset(outerRadiuses, 0, sizeof(outerRadiuses));

        for(uint32_t i = 0; i < room->light_count && current_light_number < MAX_NUM_LIGHTS; i++)
        {
            current_light = &room->lights[i];

            float x = entity->transform[12] - current_light->pos[0];
            float y = entity->transform[13] - current_light->pos[1];
            float z = entity->transform[14] - current_light->pos[2];

            float distance = sqrt(x * x + y * y + z * z);

            // Find color
            colors[current_light_number*4 + 0] = std::fmin(std::fmax(current_light->colour[0], 0.0), 1.0);
            colors[current_light_number*4 + 1] = std::fmin(std::fmax(current_light->colour[1], 0.0), 1.0);
            colors[current_light_number*4 + 2] = std::fmin(std::fmax(current_light->colour[2], 0.0), 1.0);
            colors[current_light_number*4 + 3] = std::fmin(std::fmax(current_light->colour[3], 0.0), 1.0);

            if(room->flags & TR_ROOM_FLAG_WATER)
            {
                CalculateWaterTint(colors + current_light_number * 4, 0);
            }

            // Find position
            Mat4_vec3_mul(&positions[3*current_light_number], modelViewMatrix, current_light->pos);

            // Find fall-off
            if(current_light->light_type == LT_SUN)
            {
                innerRadiuses[current_light_number] = 1e20f;
                outerRadiuses[current_light_number] = 1e21f;
                current_light_number++;
            }
            else if(distance <= current_light->outer + 1024.0f && (current_light->light_type == LT_POINT || current_light->light_type == LT_SHADOW))
            {
                innerRadiuses[current_light_number] = std::fabs(current_light->inner);
                outerRadiuses[current_light_number] = std::fabs(current_light->outer);
                current_light_number++;
            }
        }

        shader = shaderManager->getEntityShader(current_light_number);
        glUseProgramObjectARB(shader->program);
        glUniform4fvARB(shader->light_ambient, 1, ambient_component);
        glUniform4fvARB(shader->light_color, current_light_number, colors);
        glUniform3fvARB(shader->light_position, current_light_number, positions);
        glUniform1fvARB(shader->light_inner_radius, current_light_number, innerRadiuses);
        glUniform1fvARB(shader->light_outer_radius, current_light_number, outerRadiuses);
    }
    else
    {
        shader = shaderManager->getEntityShader(0);
        glUseProgramObjectARB(shader->program);
    }
    return shader;
}

/**
 * DEBUG PRIMITIVES RENDERING
 */
CRenderDebugDrawer::CRenderDebugDrawer():
m_drawFlags(0x00000000),
m_lines(0),
m_max_lines(DEBUG_DRAWER_DEFAULT_BUFFER_SIZE),
m_gl_vbo(0),
m_need_realloc(false),
m_obb(NULL),
m_buffer(NULL)
{
    m_buffer = (GLfloat*)malloc(2 * 6 * m_max_lines * sizeof(GLfloat));
    vec3_set_zero(m_color);
    m_obb = OBB_Create();
}

CRenderDebugDrawer::~CRenderDebugDrawer()
{
    free(m_buffer);
    m_buffer = NULL;
    if(m_gl_vbo != 0)
    {
        glDeleteBuffersARB(1, &m_gl_vbo);
        m_gl_vbo = 0;
    }
    OBB_Clear(m_obb);
    m_obb = NULL;
}

void CRenderDebugDrawer::Reset()
{
    if(m_need_realloc)
    {
        uint32_t new_buffer_size = m_max_lines * 12 * 2;
        GLfloat *new_buffer = (GLfloat*)malloc(new_buffer_size * sizeof(GLfloat));
        if(new_buffer != NULL)
        {
            free(m_buffer);
            m_buffer = new_buffer;
            m_max_lines *= 2;
        }
        m_need_realloc = false;
    }
    if(m_gl_vbo == 0)
    {
        glGenBuffersARB(1, &m_gl_vbo);
    }
    m_lines = 0;
}

void CRenderDebugDrawer::Render()
{
    if((m_lines > 0) && (m_gl_vbo != 0))
    {
        glBindBufferARB(GL_ARRAY_BUFFER_ARB, m_gl_vbo);
        glBufferDataARB(GL_ARRAY_BUFFER_ARB, m_lines * 12 * sizeof(GLfloat), m_buffer, GL_STREAM_DRAW);
        glVertexPointer(3, GL_FLOAT, 6 * sizeof(GLfloat), (void*)0);
        glColorPointer(3, GL_FLOAT, 6 * sizeof(GLfloat),  (void*)(3 * sizeof(GLfloat)));
        glDrawArrays(GL_LINES, 0, 2 * m_lines);
        glBindBufferARB(GL_ARRAY_BUFFER_ARB, 0);
    }

    vec3_set_zero(m_color);
    m_lines = 0;
}

void CRenderDebugDrawer::DrawAxis(float r, float transform[16])
{
    GLfloat *v0, *v;

    if(m_lines + 3 >= m_max_lines)
    {
        m_need_realloc = true;
        return;
    }

    v0 = v = m_buffer + 3 * 4 * m_lines;
    m_lines += 3;
    vec3_copy(v0, transform + 12);

    // OX
    v += 3;
    v[0] = 1.0;
    v[1] = 0.0;
    v[2] = 0.0;
    v += 3;
    vec3_add_mul(v, v0, transform + 0, r);
    v += 3;
    v[0] = 1.0;
    v[1] = 0.0;
    v[2] = 0.0;
    v += 3;

    // OY
    vec3_copy(v, v0);
    v += 3;
    v[0] = 0.0;
    v[1] = 1.0;
    v[2] = 0.0;
    v += 3;
    vec3_add_mul(v, v0, transform + 4, r);
    v += 3;
    v[0] = 0.0;
    v[1] = 1.0;
    v[2] = 0.0;
    v += 3;

    // OZ
    vec3_copy(v, v0);
    v += 3;
    v[0] = 0.0;
    v[1] = 0.0;
    v[2] = 1.0;
    v += 3;
    vec3_add_mul(v, v0, transform + 8, r);
    v += 3;
    v[0] = 0.0;
    v[1] = 0.0;
    v[2] = 1.0;
}

void CRenderDebugDrawer::DrawFrustum(struct frustum_s *f)
{
    if(f != NULL)
    {
        GLfloat *v, *v0;
        float *fv = f->vertex;

        if(m_lines + f->vertex_count >= m_max_lines)
        {
            m_need_realloc = true;
            return;
        }

        v = v0 = m_buffer + 3 * 4 * m_lines;
        m_lines += f->vertex_count;

        for(uint16_t i=0;i<f->vertex_count-1;i++,fv += 3)
        {
            vec3_copy(v, fv);
            v += 3;
            vec3_copy(v, m_color);
            v += 3;

            vec3_copy(v, fv + 3);
            v += 3;
            vec3_copy(v, m_color);
            v += 3;
        }

        vec3_copy(v, fv);
        v += 3;
        vec3_copy(v, m_color);
        v += 3;
        vec3_copy(v, v0);
        v += 3;
        vec3_copy(v, m_color);
    }
}

void CRenderDebugDrawer::DrawPortal(struct portal_s *p)
{
    if(p != NULL)
    {
        GLfloat *v, *v0;
        float *pv = p->vertex;

        if(m_lines + p->vertex_count >= m_max_lines)
        {
            m_need_realloc = true;
            return;
        }

        v = v0 = m_buffer + 3 * 4 * m_lines;
        m_lines += p->vertex_count;

        for(uint16_t i=0;i<p->vertex_count-1;i++,pv += 3)
        {
            vec3_copy(v, pv);
            v += 3;
            vec3_copy(v, m_color);
            v += 3;

            vec3_copy(v, pv + 3);
            v += 3;
            vec3_copy(v, m_color);
            v += 3;
        }

        vec3_copy(v, pv);
        v += 3;
        vec3_copy(v, m_color);
        v += 3;
        vec3_copy(v, v0);
        v += 3;
        vec3_copy(v, m_color);
    }
}

void CRenderDebugDrawer::DrawBBox(float bb_min[3], float bb_max[3], float *transform)
{
    if(m_lines + 12 < m_max_lines)
    {
        OBB_Rebuild(m_obb, bb_min, bb_max);
        m_obb->transform = transform;
        OBB_Transform(m_obb);
        this->DrawOBB(m_obb);
    }
    else
    {
        m_need_realloc = true;
    }
}

void CRenderDebugDrawer::DrawOBB(struct obb_s *obb)
{
    GLfloat *v, *v0;
    polygon_p p = obb->polygons;

    if(m_lines + 12 >= m_max_lines)
    {
        m_need_realloc = true;
        return;
    }

    v = v0 = m_buffer + 3 * 4 * m_lines;
    m_lines += 12;

    vec3_copy(v, p->vertices[0].position);
    v += 3;
    vec3_copy(v, m_color);
    v += 3;
    vec3_copy(v, (p+1)->vertices[0].position);
    v += 3;
    vec3_copy(v, m_color);
    v += 3;

    vec3_copy(v, p->vertices[1].position);
    v += 3;
    vec3_copy(v, m_color);
    v += 3;
    vec3_copy(v, (p+1)->vertices[3].position);
    v += 3;
    vec3_copy(v, m_color);
    v += 3;

    vec3_copy(v, p->vertices[2].position);
    v += 3;
    vec3_copy(v, m_color);
    v += 3;
    vec3_copy(v, (p+1)->vertices[2].position);
    v += 3;
    vec3_copy(v, m_color);
    v += 3;

    vec3_copy(v, p->vertices[3].position);
    v += 3;
    vec3_copy(v, m_color);
    v += 3;
    vec3_copy(v, (p+1)->vertices[1].position);
    v += 3;
    vec3_copy(v, m_color);
    v += 3;

    for(uint16_t i=0; i<2; i++,p++)
    {
        vertex_p pv = p->vertices;
        v0 = v;
        for(uint16_t j=0;j<p->vertex_count-1;j++,pv++)
        {
            vec3_copy(v, pv->position);
            v += 3;
            vec3_copy(v, m_color);
            v += 3;

            vec3_copy(v, (pv+1)->position);
            v += 3;
            vec3_copy(v, m_color);
            v += 3;
        }

        vec3_copy(v, pv->position);
        v += 3;
        vec3_copy(v, m_color);
        v += 3;
        vec3_copy(v, v0);
        v += 3;
        vec3_copy(v, m_color);
        v += 3;
    }
}

void CRenderDebugDrawer::DrawMeshDebugLines(struct base_mesh_s *mesh, float transform[16], const float *overrideVertices, const float *overrideNormals)
{
    if((!m_need_realloc) && (m_drawFlags & R_DRAW_NORMALS))
    {
        GLfloat *v = m_buffer + 3 * 4 * m_lines;
        float n[3];

        if(m_lines + mesh->vertex_count >= m_max_lines)
        {
            m_need_realloc = true;
            return;
        }

        this->SetColor(0.8, 0.0, 0.9);
        m_lines += mesh->vertex_count;
        if(overrideVertices)
        {
            float *ov = (float*)overrideVertices;
            float *on = (float*)overrideNormals;
            for(uint32_t i=0; i<mesh->vertex_count; i++,ov+=3,on+=3,v+=12)
            {
                Mat4_vec3_mul_macro(v, transform, ov);
                Mat4_vec3_rot_macro(n, transform, on);

                v[6 + 0] = v[0] + n[0] * 128.0;
                v[6 + 1] = v[1] + n[1] * 128.0;
                v[6 + 2] = v[2] + n[2] * 128.0;
                vec3_copy(v+3, m_color);
                vec3_copy(v+9, m_color);
            }
        }
        else
        {
            vertex_p mv = mesh->vertices;
            for (uint32_t i = 0; i < mesh->vertex_count; i++,mv++,v+=12)
            {
                Mat4_vec3_mul_macro(v, transform, mv->position);
                Mat4_vec3_rot_macro(n, transform, mv->normal);

                v[6 + 0] = v[0] + n[0] * 128.0;
                v[6 + 1] = v[1] + n[1] * 128.0;
                v[6 + 2] = v[2] + n[2] * 128.0;
                vec3_copy(v+3, m_color);
                vec3_copy(v+9, m_color);
            }
        }
    }
}

void CRenderDebugDrawer::DrawSkeletalModelDebugLines(struct ss_bone_frame_s *bframe, float transform[16])
{
    if((!m_need_realloc) && m_drawFlags & R_DRAW_NORMALS)
    {
        float tr[16];

        ss_bone_tag_p btag = bframe->bone_tags;
        for(uint16_t i=0; i<bframe->bone_tag_count; i++,btag++)
        {
            Mat4_Mat4_mul(tr, transform, btag->full_transform);
            this->DrawMeshDebugLines(btag->mesh_base, tr, NULL, NULL);
        }
    }
}

void CRenderDebugDrawer::DrawEntityDebugLines(struct entity_s *entity)
{
    if(m_need_realloc || entity->was_rendered_lines || !(m_drawFlags & (R_DRAW_AXIS | R_DRAW_NORMALS | R_DRAW_BOXES)) ||
       !(entity->state_flags & ENTITY_STATE_VISIBLE) || (entity->bf->animations.model->hide && !(m_drawFlags & R_DRAW_NULLMESHES)))
    {
        return;
    }

    if(m_drawFlags & R_DRAW_BOXES)
    {
        this->SetColor(0.0, 0.0, 1.0);
        this->DrawOBB(entity->obb);
    }

    if(m_drawFlags & R_DRAW_AXIS)
    {
        // If this happens, the lines after this will get drawn with random colors. I don't care.
        this->DrawAxis(1000.0, entity->transform);
    }

    if(entity->bf->animations.model && entity->bf->animations.model->animations)
    {
        this->DrawSkeletalModelDebugLines(entity->bf, entity->transform);
    }

    entity->was_rendered_lines = 1;
}

void CRenderDebugDrawer::DrawSectorDebugLines(struct room_sector_s *rs)
{
    if(m_lines + 12 < m_max_lines)
    {
        float bb_min[3] = {(float)(rs->pos[0] - TR_METERING_SECTORSIZE / 2.0), (float)(rs->pos[1] - TR_METERING_SECTORSIZE / 2.0), (float)rs->floor};
        float bb_max[3] = {(float)(rs->pos[0] + TR_METERING_SECTORSIZE / 2.0), (float)(rs->pos[1] + TR_METERING_SECTORSIZE / 2.0), (float)rs->ceiling};

        this->DrawBBox(bb_min, bb_max, NULL);
    }
    else
    {
        m_need_realloc = true;
    }
}

void CRenderDebugDrawer::DrawRoomDebugLines(struct room_s *room, struct camera_s *cam)
{
    frustum_p frus;
    engine_container_p cont;
    entity_p ent;

    if(m_need_realloc)
    {
        return;
    }

    if(m_drawFlags & R_DRAW_ROOMBOXES)
    {
        this->SetColor(0.0, 0.1, 0.9);
        this->DrawBBox(room->bb_min, room->bb_max, NULL);
        /*for(uint32_t s=0;s<room->sectors_count;s++)
        {
            drawSectorDebugLines(room->sectors + s);
        }*/
    }

    if(m_drawFlags & R_DRAW_PORTALS)
    {
        this->SetColor(0.0, 0.0, 0.0);
        for(uint16_t i=0; i<room->portal_count; i++)
        {
            this->DrawPortal(room->portals+i);
        }
    }

    if(m_drawFlags & R_DRAW_FRUSTUMS)
    {
        this->SetColor(1.0, 0.0, 0.0);
        for(frus=room->frustum; frus; frus=frus->next)
        {
            this->DrawFrustum(frus);
        }
    }

    if(!(m_drawFlags & R_SKIP_ROOM) && (room->mesh != NULL))
    {
        this->DrawMeshDebugLines(room->mesh, room->transform, NULL, NULL);
    }

    bool draw_boxes = m_drawFlags & R_DRAW_BOXES;
    for(uint32_t i=0; i<room->static_mesh_count; i++)
    {
        if(room->static_mesh[i].was_rendered_lines || !Frustum_IsOBBVisibleInFrustumList(room->static_mesh[i].obb, (room->frustum)?(room->frustum):(cam->frustum)) ||
          ((room->static_mesh[i].hide == 1) && !(m_drawFlags & R_DRAW_DUMMY_STATICS)))
        {
            continue;
        }

        if(draw_boxes)
        {
            this->SetColor(0.0, 1.0, 0.1);
            this->DrawOBB(room->static_mesh[i].obb);
        }

        if(m_drawFlags & R_DRAW_AXIS)
        {
            this->DrawAxis(1000.0, room->static_mesh[i].transform);
        }

        this->DrawMeshDebugLines(room->static_mesh[i].mesh, room->static_mesh[i].transform, NULL, NULL);

        room->static_mesh[i].was_rendered_lines = 1;
    }

    for(cont=room->containers; cont; cont=cont->next)
    {
        switch(cont->object_type)
        {
        case OBJECT_ENTITY:
            ent = (entity_p)cont->object;
            if(ent->was_rendered_lines == 0)
            {
                if(Frustum_IsOBBVisibleInFrustumList(ent->obb, (room->frustum)?(room->frustum):(cam->frustum)))
                {
                    this->DrawEntityDebugLines(ent);
                }
                ent->was_rendered_lines = 1;
            }
            break;
        };
    }
}

void CRenderDebugDrawer::DrawLine(const float from[3], const float to[3], const float color_from[3], const float color_to[3])
{
    GLfloat *v;

    if(m_lines < m_max_lines - 1)
    {
        v = m_buffer + 3 * 4 * m_lines;
        m_lines++;

        vec3_copy(v, from);
        v += 3;
        vec3_copy(v, color_from);
        v += 3;
        vec3_copy(v, to);
        v += 3;
        vec3_copy(v, color_to);
    }
    else
    {
        m_need_realloc = true;
    }
}

void CRenderDebugDrawer::DrawContactPoint(const float pointOnB[3], const float normalOnB[3], float distance, int lifeTime, const float color[3])
{
    if(m_lines + 2 < m_max_lines)
    {
        float to[3];
        GLfloat *v = m_buffer + 3 * 4 * m_lines;

        m_lines += 2;
        vec3_add_mul(to, pointOnB, normalOnB, distance);

        vec3_copy(v, pointOnB);
        v += 3;
        vec3_copy(v, color);
        v += 3;

        vec3_copy(v, to);
        v += 3;
        vec3_copy(v, color);

        //glRasterPos3f(from.x(),  from.y(),  from.z());
        //char buf[12];
        //sprintf(buf," %d",lifeTime);
        //BMF_DrawString(BMF_GetFont(BMF_kHelvetica10),buf);
    }
    else
    {
        m_need_realloc = true;
    }
}

/*
 * Other functions
 */
void CalculateWaterTint(GLfloat *tint, uint8_t fixed_colour)
{
    if(engine_world.version < TR_IV)  // If water room and level is TR1-3
    {
        if(engine_world.version < TR_III)
        {
             // Placeholder, color very similar to TR1 PSX ver.
            if(fixed_colour > 0)
            {
                tint[0] = 0.585f;
                tint[1] = 0.9f;
                tint[2] = 0.9f;
                tint[3] = 1.0f;
            }
            else
            {
                tint[0] *= 0.585f;
                tint[1] *= 0.9f;
                tint[2] *= 0.9f;
            }
        }
        else
        {
            // TOMB3 - closely matches TOMB3
            if(fixed_colour > 0)
            {
                tint[0] = 0.275f;
                tint[1] = 0.45f;
                tint[2] = 0.5f;
                tint[3] = 1.0f;
            }
            else
            {
                tint[0] *= 0.275f;
                tint[1] *= 0.45f;
                tint[2] *= 0.5f;
            }
        }
    }
    else
    {
        if(fixed_colour > 0)
        {
            tint[0] = 1.0f;
            tint[1] = 1.0f;
            tint[2] = 1.0f;
            tint[3] = 1.0f;
        }
    }
}