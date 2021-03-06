#include <Windows.h>
#include <stdio.h>
#include <string>

EXTERN_C
{
#include "xxhash.h"
#include "sph_blake2s.h"
}


#pragma pack(1)
struct PbdHeader
{
	DWORD Magic;
	BYTE  CompressName; //'n', '4', '\0'
	BYTE  CheckByte[3];
	DWORD Seed;
	WORD  CryptoType;
	WORD  PrivateIVLength;
};
#pragma pack()


struct ChaCha_Ctx
{
	DWORD Input[16];
};


struct PbdCryptoCompress
{
	BOOLEAN        IsBEMachine;  //+6

	DWORD          CompressType; //+8
	DWORD          CryptoType;   //+12
	DWORD          Seed;    //+16
	std::wstring   iv;      //+20

	///copy from original implementation
	void*          vTablePtr;
	DWORD          Pad1;
	DWORD          Pad2;
	DWORD          Arr[6];
	DWORD          Key;
	DWORD          ExtraRound;
	DWORD          Round;
	ULARGE_INTEGER Counter;
	ChaCha_Ctx     ChaChaContext;

	//offset + 0xc
	union
	{
		DWORD          KeyBaseStream[16];
		DWORD          KeyExtraStream[16];
		DWORD          KeyStream[32];
	} M2KeyStream;

};

template<class T>
static T SwapEndian(const T& b)
{
	T n;

	switch (sizeof(T))
	{
	case 8: //64-bit
		((uint8_t*)&n)[0] = ((uint8_t*)&b)[7];
		((uint8_t*)&n)[1] = ((uint8_t*)&b)[6];
		((uint8_t*)&n)[2] = ((uint8_t*)&b)[5];
		((uint8_t*)&n)[3] = ((uint8_t*)&b)[4];
		((uint8_t*)&n)[4] = ((uint8_t*)&b)[3];
		((uint8_t*)&n)[5] = ((uint8_t*)&b)[2];
		((uint8_t*)&n)[6] = ((uint8_t*)&b)[1];
		((uint8_t*)&n)[7] = ((uint8_t*)&b)[0];
		break;
	case 4: //32-bit
		((uint8_t*)&n)[0] = ((uint8_t*)&b)[3];
		((uint8_t*)&n)[1] = ((uint8_t*)&b)[2];
		((uint8_t*)&n)[2] = ((uint8_t*)&b)[1];
		((uint8_t*)&n)[3] = ((uint8_t*)&b)[0];
		break;
	case 2: //16-bit
		((uint8_t*)&n)[0] = ((uint8_t*)&b)[1];
		((uint8_t*)&n)[1] = ((uint8_t*)&b)[0];
		break;
	default:
		assert(0);  //Endian swap is only defined for 2, 4, and 8-byte types
		break;
	}

	return n;
}

BOOL CheckAndInit(PbdCryptoCompress* Info, FILE* Stream, LPCWSTR CustomIV = nullptr)
{
	PbdHeader Header;
	WCHAR     IVString[400];
	ULONG     CompressType;
	BOOL      CompressFoundName;

	RtlZeroMemory(IVString, sizeof(IVString));
	fread(&Header, 1, sizeof(Header), Stream);
	
	switch (Header.Magic)
	{
	case 0x2F534A54: //le machine
		Info->IsBEMachine = false;
		break;

	case 0x5C534A54: //be machine
		Info->IsBEMachine = true;
		break;

	default:
		return FALSE;
	}

	if (Header.CheckByte[0] != 0x73)
		return FALSE;

	if (Header.CheckByte[1] != 0x30)
		return FALSE;

	if (Header.CheckByte[2] != 0)
		return FALSE;

	//compress name to compress type
	static BYTE CompressNameToType[3] = {0x6E, 0x34, 0x00};

	CompressFoundName = FALSE;
	for (CompressType = 0; CompressType < _countof(CompressNameToType); CompressType++)
	{
		if (Header.CompressName == CompressNameToType[CompressType])
		{
			CompressFoundName = TRUE;
			break;
		}
	}

	if (!CompressFoundName)
		return FALSE;

	Info->CompressType = CompressType;
	Info->CryptoType = Header.CryptoType;
	if (Info->IsBEMachine)
	{
		Info->CryptoType = SwapEndian(Info->CryptoType);
		Header.PrivateIVLength = SwapEndian(Header.PrivateIVLength);
	}
	
	if (Header.PrivateIVLength)
	{
		fread(IVString, 1, Header.PrivateIVLength, Stream);
		Info->iv.clear();
		Info->iv = IVString;
	}
	else
	{
		Info->iv.clear();
		if (CustomIV)
			Info->iv = CustomIV;
		else
			Info->iv = L"RIDDLE JOKER"; //hard-coded or read from somewhere?
		                                //pls check this iv in script (via disasm or decoding)
		                                //tracing vm stack is more difficult than native code
		                                //this iv should be equal to product name(check it in AppConfig.tjs)
	}

}


///Chacha const : different from original implementation
///               as well as 'input' initialzation
void ChachaSetKeyIv(ChaCha_Ctx* ctx, BYTE* HashKey, BYTE* ChaChaConst, DWORD K1, DWORD K2, DWORD K3, DWORD K4)
{
	ctx->Input[4] = ~load32(&HashKey[0]);
	ctx->Input[5] = ~load32(&HashKey[4]);
	ctx->Input[6] = ~load32(&HashKey[8]);
	ctx->Input[7] = ~load32(&HashKey[12]);
	ctx->Input[8] = ~load32(&HashKey[16]);
	ctx->Input[9] = ~load32(&HashKey[20]);
	ctx->Input[10] = ~load32(&HashKey[24]);
	ctx->Input[11] = ~load32(&HashKey[28]);

	ctx->Input[0] = load32(&ChaChaConst[0]);
	ctx->Input[1] = load32(&ChaChaConst[4]);
	ctx->Input[2] = load32(&ChaChaConst[8]);
	ctx->Input[3] = load32(&ChaChaConst[12]);

	ctx->Input[14] = ~K1;
	ctx->Input[15] = ~K2;

	ctx->Input[12] = ~K3;
	ctx->Input[13] = ~K4;
}


void BaseFilterInit(PbdCryptoCompress* Info, DWORD Value)
{

}

//chacha8, chacha12, chacha20
//blake2s
//xxhash
BOOL InitCryptoInternal(PbdCryptoCompress* Info, DWORD Seed, DWORD Round, DWORD ExtraRound)
{
	blake2s_param hash_param;
	blake2s_state hash_status;
	DWORD         hash_input[16];
	BYTE          hash_key[32];
	DWORD         iv_hash;

	BaseFilterInit(Info, ExtraRound << 6);

	RtlZeroMemory(&hash_param,  sizeof(hash_param));
	RtlZeroMemory(&hash_status, sizeof(hash_status));
	hash_param.digest_length = 32;
	hash_param.key_length = 4;
	hash_param.fanout = 1;
	hash_param.depth = 1;

	RtlZeroMemory(hash_input, sizeof(hash_input));
	hash_input[0] = Seed;

	RtlZeroMemory(hash_key, sizeof(hash_key));

	blake2s_init_param(&hash_status, &hash_param);
	blake2s_update(&hash_status, (uint8_t*)hash_input, sizeof(hash_input));
	blake2s_update(&hash_status, (uint8_t*)Info->iv.data(), (Info->iv.length() + 1) * 2);
	blake2s_final(&hash_status, hash_key, sizeof(hash_key));

	iv_hash = XXH32(Info->iv.data(), (Info->iv.length() + 1) * 2, Seed);

	static BYTE ChachaConst[16]
		= { 0x9A, 0x87, 0x8F, 0x9E, 0x91, 0x9B, 0xDF, 0xCC, 0xCD, 0xD2, 0x9D, 0x86, 0x8B, 0x9A, 0xDF, 0x94 };

	ChachaSetKeyIv(&Info->ChaChaContext, hash_key, ChachaConst, iv_hash, Seed, 0, 0);

	Info->vTablePtr  = nullptr;
	Info->Counter.QuadPart = 0;
	Info->Round      = Round;
	Info->ExtraRound = ExtraRound;
	Info->Key        = -1;

	if (ExtraRound <= 1)
		return TRUE;

	if (iv_hash ^ Seed)
		Info->Key = iv_hash ^ Seed;
	else
		Info->Key = Seed; //nice:) avoid wake key

	return TRUE;
}

BOOL InitCrypto(PbdCryptoCompress* Info, DWORD Seed, DWORD Type)
{
	switch (Type)
	{
	case 1:
		InitCryptoInternal(Info, Seed, 8, 16); //round: for chacha crypto
		break;                                 //extra: private implementation?

	case 2:
		InitCryptoInternal(Info, Seed, 12, 8);
		break;

	case 3:
		InitCryptoInternal(Info, Seed, 20, 4);
		break;

	case 4:
		InitCryptoInternal(Info, Seed, 8, 1);
		break;
	case 5:
		InitCryptoInternal(Info, Seed, 12, 1);
		break;

	case 6:
		InitCryptoInternal(Info, Seed, 20, 1);
		break;

	default:
		printf("Unsupported crypto type.\n");
		return FALSE;
	}
}


#define ROTL32(v, n) _lrotl(v, n)
#define ROTR32(v, n) _lrotr(v, n)
#define ROTL64(v, n) _rotl64(v, n)
#define ROTR64(v, n) _rotr64(v, n)

#define ROTATE(v,c) (ROTL32(v,c))
#define XOR(v,w) ((v) ^ (w))
#define PLUS(v,w) (U32V((v) + (w)))
#define PLUSONE(v) (PLUS((v),1))

#define QUARTERROUND(a,b,c,d) \
  a = PLUS(a,b); d = ROTATE(XOR(d,a),16); \
  c = PLUS(c,d); b = ROTATE(XOR(b,c),12); \
  a = PLUS(a,b); d = ROTATE(XOR(d,a), 8); \
  c = PLUS(c,d); b = ROTATE(XOR(b,c), 7);



__declspec(naked) void m2_chacha_base_stream_asm()
{
	__asm
	{
                mov     ecx, [esp+8]
                sub     esp, 40h
                push    ebx
                push    ebp
                push    esi
                mov     edx, ecx
                lea     esi, [esp+0Ch]
                xor     eax, eax
                push    edi
                sub     edx, esi

loc_100474A5:                           ; CODE XREF: wordtobyte+26↓j
                lea     esi, [edx+eax*4]
                mov     esi, [esp+esi+10h]
                not     esi
                mov     [esp+eax*4+10h], esi
                inc     eax
                cmp     eax, 10h
                jl      short loc_100474A5
                mov     eax, [esp+5Ch]
                test    eax, eax
                jle     loc_100476DD
                mov     esi, [esp+40h]
                mov     ebp, [esp+30h]
                mov     ecx, [esp+20h]
                mov     ebx, [esp+10h]
                dec     eax
                shr     eax, 1
                inc     eax
                mov     [esp+5Ch], eax
                lea     esp, [esp+0]

loc_100474E0:                           ; CODE XREF: wordtobyte+23B↓j
                add     ebx, ecx
                mov     eax, esi
                xor     eax, ebx
                rol     eax, 10h
                add     ebp, eax
                mov     edx, [esp+34h]
                mov     edi, ebp
                xor     edi, ecx
                rol     edi, 0Ch
                add     ebx, edi
                xor     eax, ebx
                rol     eax, 8
                mov     ecx, [esp+44h]
                add     ebp, eax
                mov     [esp+40h], eax
                mov     eax, ebp
                xor     eax, edi
                rol     eax, 7
                mov     [esp+20h], eax
                mov     eax, [esp+14h]
                add     eax, [esp+24h]
                mov     esi, [esp+28h]
                xor     ecx, eax
                rol     ecx, 10h
                add     edx, ecx
                mov     edi, edx
                xor     edi, [esp+24h]
                rol     edi, 0Ch
                add     eax, edi
                xor     ecx, eax
                rol     ecx, 8
                add     edx, ecx
                mov     [esp+44h], ecx
                mov     ecx, edx
                xor     ecx, edi
                mov     edi, [esp+38h]
                rol     ecx, 7
                mov     [esp+34h], edx
                mov     edx, [esp+48h]
                mov     [esp+24h], ecx
                mov     ecx, [esp+18h]
                add     ecx, esi
                xor     edx, ecx
                rol     edx, 10h
                add     edi, edx
                mov     [esp+38h], edi
                xor     edi, esi
                mov     esi, [esp+38h]
                rol     edi, 0Ch
                add     ecx, edi
                xor     edx, ecx
                rol     edx, 8
                add     esi, edx
                mov     [esp+48h], edx
                mov     edx, esi
                xor     edx, edi
                mov     edi, [esp+2Ch]
                rol     edx, 7
                mov     [esp+28h], edx
                mov     edx, [esp+1Ch]
                add     edx, edi
                mov     [esp+38h], esi
                mov     esi, [esp+4Ch]
                xor     esi, edx
                mov     [esp+1Ch], edx
                mov     edx, [esp+3Ch]
                rol     esi, 10h
                add     edx, esi
                mov     [esp+3Ch], edx
                xor     edx, edi
                mov     edi, [esp+1Ch]
                rol     edx, 0Ch
                add     edi, edx
                xor     esi, edi
                mov     [esp+1Ch], edi
                mov     edi, [esp+3Ch]
                rol     esi, 8
                add     edi, esi
                mov     [esp+3Ch], edi
                xor     edi, edx
                rol     edi, 7
                add     ebx, [esp+24h]
                add     eax, [esp+28h]
                mov     edx, [esp+38h]
                xor     esi, ebx
                rol     esi, 10h
                add     edx, esi
                mov     [esp+38h], edx
                xor     edx, [esp+24h]
                mov     [esp+4Ch], esi
                rol     edx, 0Ch
                add     ebx, edx
                mov     [esp+24h], edx
                mov     edx, esi
                mov     esi, [esp+38h]
                xor     edx, ebx
                rol     edx, 8
                add     esi, edx
                mov     [esp+4Ch], edx
                mov     edx, eax
                xor     edx, [esp+40h]
                mov     [esp+38h], esi
                xor     esi, [esp+24h]
                add     ecx, edi
                rol     esi, 7
                rol     edx, 10h
                mov     [esp+40h], edx
                mov     [esp+24h], esi
                mov     esi, [esp+3Ch]
                add     esi, edx
                mov     edx, esi
                xor     edx, [esp+28h]
                mov     [esp+3Ch], esi
                rol     edx, 0Ch
                add     eax, edx
                mov     [esp+14h], eax
                xor     eax, [esp+40h]
                rol     eax, 8
                mov     esi, eax
                mov     eax, [esp+3Ch]
                add     eax, esi
                mov     [esp+3Ch], eax
                xor     eax, edx
                rol     eax, 7
                mov     edx, ecx
                xor     edx, [esp+44h]
                mov     [esp+28h], eax
                rol     edx, 10h
                add     ebp, edx
                xor     edi, ebp
                rol     edi, 0Ch
                add     ecx, edi
                mov     [esp+18h], ecx
                xor     ecx, edx
                rol     ecx, 8
                mov     eax, ecx
                mov     ecx, [esp+34h]
                add     ebp, eax
                mov     [esp+44h], eax
                mov     eax, [esp+1Ch]
                add     eax, [esp+20h]
                xor     edi, ebp
                mov     edx, eax
                xor     edx, [esp+48h]
                rol     edi, 7
                rol     edx, 10h
                add     ecx, edx
                mov     [esp+2Ch], edi
                mov     edi, ecx
                xor     edi, [esp+20h]
                mov     [esp+40h], esi
                rol     edi, 0Ch
                add     eax, edi
                mov     [esp+1Ch], eax
                xor     eax, edx
                rol     eax, 8
                add     ecx, eax
                mov     [esp+34h], ecx
                xor     ecx, edi
                rol     ecx, 7
                mov     [esp+48h], eax
                mov     [esp+20h], ecx
                sub     dword ptr [esp+5Ch], 1
                jnz     loc_100474E0
                mov     ecx, [esp+58h]
                mov     [esp+30h], ebp
                mov     [esp+10h], ebx

loc_100476DD:                           ; CODE XREF: wordtobyte+2E↑j
                mov     eax, [esp+54h]
                lea     ebp, [esp+10h]
                lea     edx, [esp+14h]
                xor     edi, edi
                sub     ebp, ecx
                lea     esi, [ecx+8]
                sub     edx, ecx

loc_100476F2:                           ; CODE XREF: wordtobyte+2F5↓j
                mov     ecx, [esi-8]
                not     ecx
                add     ecx, [esp+edi*4+10h]
                add     edi, 4
                mov     ebx, ecx
                shr     ebx, 8
                mov     [eax+1], bl
                mov     ebx, ecx
                mov     [eax], cl
                shr     ebx, 10h
                mov     [eax+2], bl
                shr     ecx, 18h
                mov     [eax+3], cl
                mov     ecx, [esi-4]
                not     ecx
                add     ecx, [esp+edi*4+4]
                add     esi, 10h
                mov     ebx, ecx
                shr     ebx, 8
                mov     [eax+5], bl
                mov     ebx, ecx
                mov     [eax+4], cl
                shr     ebx, 10h
                shr     ecx, 18h
                mov     [eax+6], bl
                mov     [eax+7], cl
                mov     ecx, [esi-10h]
                not     ecx
                add     ecx, [esi+ebp-10h]
                add     eax, 10h
                mov     ebx, ecx
                shr     ebx, 8
                mov     [eax-7], bl
                mov     ebx, ecx
                mov     [eax-8], cl
                shr     ebx, 10h
                shr     ecx, 18h
                mov     [eax-6], bl
                mov     [eax-5], cl
                mov     ecx, [esi-0Ch]
                not     ecx
                add     ecx, [edx+esi-10h]
                mov     ebx, ecx
                shr     ebx, 8
                mov     [eax-3], bl
                mov     ebx, ecx
                mov     [eax-4], cl
                shr     ebx, 10h
                shr     ecx, 18h
                cmp     edi, 10h
                mov     [eax-2], bl
                mov     [eax-1], cl
                jl      loc_100476F2
                pop     edi
                pop     esi
                pop     ebp
                pop     ebx
                add     esp, 40h
                retn
	};
}


#define USE_ASM 1

#if defined(USE_ASM)
void m2_chacha_base_stream(DWORD* KeyStream, ChaCha_Ctx* ctx, DWORD Round)
{
	PDWORD Input = ctx->Input;
	__asm
	{
		mov  eax, Round;
		push eax;
		mov  eax, Input;
		push eax;
		mov  eax, KeyStream;
		push eax;
		call m2_chacha_base_stream_asm;
		add  esp, 0xc;
	};
}

#else

void m2_chacha_base_stream(DWORD* KeyStream, ChaCha_Ctx* ctx, DWORD Round)
{
	DWORD x[16];

	DWORD v5;
	DWORD v6;
	DWORD v7;
	DWORD v8;
	DWORD v9;
	DWORD v10;
	DWORD v11;
	DWORD v12;
	DWORD v13;
	DWORD v14;
	DWORD v15;
	DWORD v16;
	DWORD v17;
	DWORD v18;
	DWORD v19;
	DWORD v20;
	DWORD v21;
	DWORD v22;
	DWORD v23;
	DWORD v24;
	DWORD v25;
	DWORD v26;
	DWORD v27;
	DWORD v28;
	DWORD v29;
	DWORD v30;
	DWORD v31;
	DWORD v32;
	DWORD v33;
	DWORD v34;
	DWORD v35;
	DWORD v36;
	DWORD v37;
	DWORD v38;
	DWORD v39;
	DWORD v40;
	DWORD v41;
	DWORD v42;
	DWORD v43;
	DWORD v44;
	DWORD v45;
	DWORD v46;
	DWORD v47;
	DWORD v48;
	DWORD v49;
	DWORD v50;
	DWORD v51;
	DWORD v52;

	for (DWORD i = 0; i < 0x10; i++)
	{
		x[i] = ~ctx->Input[i];
	}

	if (Round > 0)
	{
		/*
		for (DWORD i = Round; i > 0; i -= 2)
		{
			QUARTERROUND(x[0], x[4], x[8], x[12])
			QUARTERROUND(x[1], x[5], x[9], x[13])
			QUARTERROUND(x[2], x[6], x[10], x[14])
			QUARTERROUND(x[3], x[7], x[11], x[15])
			QUARTERROUND(x[0], x[5], x[10], x[15])
			QUARTERROUND(x[1], x[6], x[11], x[12])
			QUARTERROUND(x[2], x[7], x[8], x[13])
			QUARTERROUND(x[3], x[4], x[9], x[14])
		}
		*/

		//m2's implementation uses some template values...
		//but the round tranform should be equal to the orignal implementation

		for (DWORD i = Round; i > 0; i -= 2)
		{
			v9 = v7 + v8;
			v10 = ROTL32(v9 ^ v5, 16);
			v11 = v10 + v6;
			v12 = ROTL32(v7 ^ v11, 12);
			v13 = v12 + v9;
			v14 = ROTL32(v13 ^ v10, 8);
			v15 = v14 + v11;
			x[12] = v14;
			x[4] = ROTL32(v12 ^ v15, 7);
			v16 = ROTL32((x[5] + x[1]) ^ x[13], 16);
			v17 = v16 + x[9];
			v18 = ROTL32(x[5] ^ (v16 + x[9]), 12);
			v19 = v18 + x[5] + x[1];
			v20 = ROTL32(v19 ^ v16, 8);
			x[13] = v20;
			x[9] = v20 + v17;
			x[5] = ROTL32(v18 ^ (v20 + v17), 7);
			v21 = ROTL32((x[6] + x[2]) ^ x[14], 16);
			v22 = v21 + x[10];
			x[10] = v22;
			v23 = ROTL32(x[6] ^ v22, 12);
			v24 = v23 + x[6] + x[2];
			x[14] = ROTL32(v24 ^ v21, 8);
			x[6] = ROTL32(v23 ^ (x[14] + x[10]), 7);
			x[10] += x[14];
			v25 = (x[7] + x[3]) ^ x[15];
			x[3] += x[7];
			v26 = ROTL32(v25, 16);
			v27 = v26 + x[11];
			x[11] = v27;
			v28 = ROTL32(x[7] ^ v27, 12);
			v29 = (v28 + x[3]) ^ v26;
			x[3] += v28;
			v30 = ROTL32(v29, 8);
			v31 = v30 + x[11];
			x[11] = v31;
			v32 = ROTL32(v28 ^ v31, 7);
			v33 = x[5] + v13;
			v34 = x[6] + v19;
			v35 = ROTL32(v33 ^ v30, 16);
			v36 = v35 + x[10];
			x[10] = v36;
			v37 = ROTL32(x[5] ^ v36, 12);
			v8 = v37 + v33;
			x[5] = v37;
			v38 = ROTL32(v8 ^ v35, 8);
			v39 = v38 + x[10];
			x[15] = v38;
			x[10] = v39;
			v40 = v32 + v24;
			x[12] = ROTL32(x[12] ^ v34, 16);
			x[5] = ROTL32(x[5] ^ v39, 7);
			v41 = x[6] ^ (x[12] + x[11]);
			x[11] += x[12];
			v42 = ROTL32(v41, 12);
			x[1] = v42 + v34;
			v5 = ROTL32(x[12] ^ (v42 + v34), 8);
			x[11] += v5;
			x[6] = ROTL32(v42 ^ x[11], 7);
			v43 = ROTL32(x[13] ^ v40, 16);
			v44 = v43 + v15;
			v45 = ROTL32(v44 ^ v32, 12);
			v46 = v45 + v40;
			x[2] = v46;
			v47 = ROTL32(v43 ^ v46, 8);
			v6 = v47 + v44;
			x[13] = v47;
			v48 = ROTL32(x[14] ^ (x[4] + x[3]), 16);
			x[7] = ROTL32(v6 ^ v45, 7);
			x[12] = v5;
			v49 = ROTL32(x[4] ^ (v48 + x[9]), 12);
			v50 = v49 + x[4] + x[3];
			x[3] = v50;
			v51 = ROTL32(v48 ^ v50, 8);
			v52 = v51 + v48 + x[9];
			x[9] = v52;
			v7 = ROTL32(v49 ^ v52, 7);
			x[14] = v51;
			x[4] = v7;
		}

		//ahh
		//KMR，刚刚计算key stream的时候你偷偷改了吧
		//没有啊
		//你这个骗子绝对偷改了
		//我有什么偷改的必要么
		//我说那个KMR啊……刚刚计算key stream的时候一直不出来
		//そうだよ（便乗）
		//你这么想改就让你改个够咯
		//(未 公 开 画 面)
		x[8] = v6;
		x[0] = v8;
	}

	DWORD i = 0;
	{
		DWORD v1 = x[i] + (~ctx->Input[0]);
		//.....
	}while (i < 16);
}

#endif

void m2_chacha_generate_block(PbdCryptoCompress* Info)
{
	ChaCha_Ctx     ctx;
	ULARGE_INTEGER Counter;
	PDWORD         ekey_p1, ekey_p2, ekey_p3, ekey_p4;
	PDWORD         key_p1, key_p2, key_p3, key_p4;
	DWORD          key_1, key_2, key_3, key_4;
	DWORD          tmp1, tmp2, tmp3;
	DWORD          key_4_f, key_2_f, key_3_f;

	Counter.QuadPart = Info->Counter.QuadPart;
	Info->Counter.QuadPart++;

	RtlCopyMemory(&ctx, &Info->ChaChaContext, sizeof(ctx));
	ctx.Input[12] = ~Counter.LowPart;
	ctx.Input[13] = ~Counter.HighPart;

	m2_chacha_base_stream(Info->M2KeyStream.KeyBaseStream, &ctx, Info->Round);
	if (Info->ExtraRound <= 1)
		return;

	///expand to 2048bit key stream?
	for (DWORD i = 4 * Info->ExtraRound - 4; i > 0; i--)
	{
		key_1 = 32 * ((((*key_p1 << 13) ^ *key_p1) >> 17) ^ (*key_p1 << 13) ^ *key_p1) ^ (((*key_p1 << 13) ^ *key_p1) >> 17) ^ (*key_p1 << 13) ^ *key_p1;
		if (!key_1)
			key_1 = Info->Key;
		*ekey_p1 = key_1;
		key_2 = key_p1[1];
		key_p2 = key_p1 + 1;
		tmp1 = (((key_2 << 13) ^ key_2) >> 17) ^ (key_2 << 13) ^ key_2;
		ekey_p2 = ekey_p1 + 1;
		key_2_f = 32 * tmp1 ^ tmp1;
		if (!key_2_f)
			key_2_f = Info->Key;
		*ekey_p2 = key_2_f;
		key_3 = key_p2[1];
		key_p3 = key_p2 + 1;
		tmp2 = (((key_3 << 13) ^ key_3) >> 17) ^ (key_3 << 13) ^ key_3;
		ekey_p3 = ekey_p2 + 1;
		key_3_f = 32 * tmp2 ^ tmp2;
		if (!key_3_f)
			key_3_f = Info->Key;
		*ekey_p3 = key_3_f;
		key_4 = key_p3[1];
		key_p4 = key_p3 + 1;
		tmp3 = (((key_4 << 13) ^ key_4) >> 17) ^ (key_4 << 13) ^ key_4;
		ekey_p4 = ekey_p3 + 1;
		key_4_f = 32 * tmp3 ^ tmp3;
		if (!key_4_f)
			key_4_f = Info->Key;
		*ekey_p4 = key_4_f;

		ekey_p1 = ekey_p4 + 1;
		key_p1 = key_p4 + 1;
	}
}


BOOL InitCryptoAndCompress(PbdCryptoCompress* Info)
{
	InitCrypto(Info, Info->Seed, Info->CryptoType);
}




int wmain(int argc, LPWSTR* argv)
{
	BOOL              Success;
	PbdCryptoCompress Info;
	FILE*             File;
	BOOL              CustomIv;
	WCHAR             Iv[200];
	CHAR              Utf8Iv[200];
	SIZE_T            Size;

	CustomIv = FALSE;
	if (argc <= 2)
		return 0;

	if (argc == 3)
	{
		CustomIv = TRUE;
		RtlZeroMemory(Iv, sizeof(Iv));
		RtlZeroMemory(Utf8Iv, sizeof(Utf8Iv));

		do
		{
			File = _wfopen(argv[2], L"rb");
			if (!File)
			{
				CustomIv = FALSE;
				break;
			}

			fseek(File, 0, SEEK_END);
			Size = ftell(File);
			rewind(File);
			fread(Utf8Iv, 1, min(sizeof(Utf8Iv) - 1, Size), File);
			fclose(File);
			File = nullptr;

			MultiByteToWideChar(CP_UTF8, 0, Utf8Iv, lstrlenA(Utf8Iv), Iv, _countof(Iv) - 1);
		} while (false);
	}

	File = _wfopen(argv[1], L"rb");

	RtlZeroMemory(&Info, sizeof(Info));
	if (CustomIv)
		Success = CheckAndInit(&Info, File, Iv);
	else
		Success = CheckAndInit(&Info, File);

	if (!Success)
	{
		printf("pbd : failed to init.\n");
		return 0;
	}

	Success = InitCryptoAndCompress(&Info);
	if (!Success)
	{
		printf("pbd : failed to crypto and compression info.\n");
		return 0;
	}

    return 0;
}

