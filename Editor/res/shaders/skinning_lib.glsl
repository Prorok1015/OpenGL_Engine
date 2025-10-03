#ifdef USE_ANIMATION

layout (std140, binding = 2) uniform BoneMatrices // TODO: change to SSBO 
{
    int rowHeight;
    int boneCount;
    mat4 bones[MAX_BONE_MATRICES_COUNT];
};

#ifdef NEW_ANIMATION

layout (std430, binding = 1) buffer IndexesBuffer
{
    uint data[];
};

out vec4 BonesLen;

mat4 culcSkinMatrix()
{
    int numColumns = int(data[0]);

    int offsetsStart = 1;
    int indicesStart = numColumns + 2;

    uint colStart = data[offsetsStart + gl_VertexID];
    uint colEnd = data[offsetsStart + gl_VertexID + 1];
    int colLength = int(colEnd - colStart) / 2;

    if (colLength < 0) {
        BonesLen.x = float(colLength);
        return uWorldMeshMatr;
    }

    mat4 skinMatrix1 = mat4(1.0);
    mat4 tmpg = mat4(0.0);
    for (int k = 0; k < colLength; ++k)
    {
        uint index = data[indicesStart + int(colStart) + k];
        uint weight = (data[indicesStart + int(colStart) + colLength + k]);
        mat4 tmp = bones[index];
        tmpg += tmp * (float(weight));
        //skinMatrix1 += tmp ;
    }


    if (colLength > 0) {
        return tmpg;
    }

    BonesLen.x = float(colLength);

    return uWorldMeshMatr;
}

#else
layout(binding = 3) uniform isampler2D boneIndicesTexture;
const int MIPMAPLVL0 = 0;
#define MAX_BONE_COUNT (boneCount < 5 ? boneCount : 4) // temporary while max bone weight == 4

int getBoneIndex(int x, int y)
{
    ivec2 texSize = textureSize(boneIndicesTexture, MIPMAPLVL0);
    int power = x / texSize.x;
    int tmpX = x - (texSize.x * power);
    int tmpY = y * rowHeight + power;
    ivec2 boneCoord = ivec2(tmpX, tmpY);
    return texelFetch(boneIndicesTexture, boneCoord, MIPMAPLVL0).r;
}

mat4 culcSkinMatrix()
{
    bool is_at_least_one_bone = false;
    mat4 skinMatrix = mat4(0.0);

    int boneIndex = -1;

    for(int i = 0; i < MAX_BONE_COUNT; ++i)
    {
        boneIndex = getBoneIndex(gl_VertexID, i);

        if(boneIndex > -1) {
            skinMatrix += bones[boneIndex] * aBonesWeight[i];
            is_at_least_one_bone = true;
        } else {
            break;
        }
    }

    if (is_at_least_one_bone)
        return skinMatrix;

    return uWorldMeshMatr;
}
#endif
#endif
