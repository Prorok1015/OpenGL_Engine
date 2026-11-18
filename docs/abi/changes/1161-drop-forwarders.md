#1161: the forwarding headers at the old paths are gone: <snk/resource/res_tag.h>, res_tag_json.h, res_asset_id.h,
res_reference.h, <snk/render/geom/rnd_geometry_encode.h> and <snk/scene/scn_glm_json_convert.h>. A module includes them as
<snk/model_codec/...>. Source only: no export or type changes, the layout key stays.
