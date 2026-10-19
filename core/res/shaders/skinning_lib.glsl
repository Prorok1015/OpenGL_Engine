#ifdef USE_ANIMATION

// The matrices of every skeleton of the frame; ObjectData::boneOffset is where this object's skeleton starts.
SNK_SSBO(3, 0) readonly buffer BoneMatrices
{
    mat4 bones[];
};

SNK_SSBO(3, 1) readonly buffer IndexesBuffer
{
    uint data[];
};

mat4 culcSkinMatrix(uint ID, mat4 defaultMatrix)
{
    // A draw whose skin has no weights is bound a buffer of zeros, and a buffer can be shorter than the mesh
    // needs: out of range the vertex keeps the matrix it came with.
    uint size = uint(data.length());
    if (size < 2u) {
        return defaultMatrix;
    }
    uint numColumns = data[0];
    if (ID >= numColumns || size < numColumns + 2u) {
        return defaultMatrix;
    }

    uint offsetsStart = 1;
    uint indicesStart = numColumns + 2;

    uint colStart = data[offsetsStart + ID];
    uint colEnd = data[offsetsStart + ID + 1];
    if (colEnd < colStart || indicesStart + colEnd > size) {
        return defaultMatrix;
    }
    uint colLength = (colEnd - colStart) / 2;

    mat4 skinMatrix = mat4(0.0);
    for (uint k = 0; k < colLength; ++k)
    {
        uint index = data[indicesStart + colStart + k];
        uint weight = data[indicesStart + colStart + colLength + k];
        skinMatrix += bones[boneOffset + index] * uintBitsToFloat(weight);
    }

    if (colLength > 0) {
        return skinMatrix;
    }

    return defaultMatrix;
}
#endif
