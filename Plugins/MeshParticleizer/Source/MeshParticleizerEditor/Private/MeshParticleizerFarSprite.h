#pragma once

class UMaterial;

// 近距離のSpriteを縮退し、遠方だけ同じ粒子の円形Spriteを描く。
// 粒子状態とGPU配列は増やさず、Spriteの頂点処理は残る。
void MeshParticleizerConfigureFarSpriteMaterial(UMaterial* Material, float Distance, bool bCircle);
