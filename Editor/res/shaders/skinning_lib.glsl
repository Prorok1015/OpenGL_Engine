#ifdef USE_ANIMATION

layout (std430, binding = 2) buffer BoneMatrices
{
    mat4 bones[];
};

layout (std430, binding = 1) buffer IndexesBuffer
{
    uint data[];
};

mat4 culcSkinMatrix(uint ID, mat4 defaultMatrix)
{
    uint numColumns = data[0];

    uint offsetsStart = 1;
    uint indicesStart = numColumns + 2;

    uint colStart = data[offsetsStart + ID];
    uint colEnd = data[offsetsStart + ID + 1];
    uint colLength = (colEnd - colStart) / 2;

    mat4 skinMatrix = mat4(0.0);
    for (uint k = 0; k < colLength; ++k)
    {
        uint index = data[indicesStart + colStart + k];
        uint weight = data[indicesStart + colStart + colLength + k];
        skinMatrix += bones[index] * uintBitsToFloat(weight);
    }

    if (colLength > 0) {
        return skinMatrix;
    }

    return defaultMatrix;
}
#endif
