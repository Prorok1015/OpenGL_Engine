#1161: res_tag.h, res_tag_json.h, res_asset_id.h, res_reference.h, rnd_geometry_encode.h and scn_glm_json_convert.h move to
<snk/model_codec/...> (snk_model_codec), with the pure part of res_desc_binary.h in <snk/model_codec/res_desc_binary_codec.h>;
the old paths forward to them until the includes move (#1161 1a/1b). Declarations and exports are the same: no type changes,
the layout key stays.
