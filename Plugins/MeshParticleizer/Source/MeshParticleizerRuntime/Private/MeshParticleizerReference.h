#pragma once

#include "CoreMinimal.h"

class USceneComponent;

namespace MeshParticleizerReference
{
	bool IsInvertible(const FMatrix& Matrix);
	bool GetSocketTransform(const USceneComponent* Component, FName Name, FMatrix& OutTransform);
}
