//
// Copyright (c) 2017-2018, Manticore Software LTD (http://manticoresearch.com)
// Copyright (c) 2001-2016, Andrew Aksyonoff
// Copyright (c) 2008-2016, Sphinx Technologies Inc
// All rights reserved
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//

#include "sphinxfilter.h"
#include "sphinxint.h"
#include "sphinxjson.h"
#include "sphinxutils.h"

#if USE_WINDOWS
#pragma warning(disable:4250) // inheritance via dominance is our intent
#endif

/// attribute-based
struct IFilter_Attr: virtual ISphFilter
{
	CSphAttrLocator m_tLocator;

	void SetLocator ( const CSphAttrLocator & tLocator ) override
	{
		m_tLocator = tLocator;
	}
};

/// values
struct IFilter_Values : virtual ISphFilter
{
	const SphAttr_t *	m_pValues = nullptr;
	int					m_iValueCount = 0;

	void SetValues ( const SphAttr_t * pStorage, int iCount ) final
	{
		assert ( pStorage );
		assert ( iCount > 0 );
		#ifndef NDEBUG // values must be sorted
		for ( int i = 1; i < iCount; i++ )
			assert ( pStorage[i-1]<=pStorage[i] );
		#endif

		m_pValues = pStorage;
		m_iValueCount = iCount;
	}

	inline SphAttr_t GetValue ( int iIndex ) const
	{
		assert ( iIndex>=0 && iIndex<m_iValueCount );
		return m_pValues[iIndex];
	}

	bool EvalValues ( SphAttr_t uValue ) const;
	bool EvalBlockValues ( SphAttr_t uBlockMin, SphAttr_t uBlockMax ) const;
};


bool IFilter_Values::EvalValues ( SphAttr_t uValue ) const
{
	if ( !m_pValues )
		return true;

	const SphAttr_t * pA = m_pValues;
	const SphAttr_t * pB = m_pValues + m_iValueCount - 1;

	if ( uValue==*pA || uValue==*pB ) return true;
	if ( uValue<(*pA) || uValue>(*pB) ) return false;

	while ( pB-pA>1 )
	{
		const SphAttr_t * pM = pA + ((pB-pA)/2);
		if ( uValue==(*pM) )
			return true;
		if ( uValue<(*pM) )
			pB = pM;
		else
			pA = pM;
	}
	return false;
}


// OPTIMIZE: use binary search
bool IFilter_Values::EvalBlockValues ( SphAttr_t uBlockMin, SphAttr_t uBlockMax ) const
{
	// is any of our values inside the block?
	for ( int i = 0; i < m_iValueCount; ++i )
		if ( GetValue(i)>=uBlockMin && GetValue(i)<=uBlockMax )
			return true;
	return false;
}


template<bool HAS_EQUAL_MIN, bool HAS_EQUAL_MAX, bool OPEN_LEFT = false, bool OPEN_RIGHT = false, typename T = SphAttr_t>
bool EvalRange ( T tValue, T tMin, T tMax )
{
	if_const ( OPEN_LEFT )
		return HAS_EQUAL_MAX ? ( tValue<=tMax ) : ( tValue<tMax );

	if_const ( OPEN_RIGHT )
		return  HAS_EQUAL_MIN ? ( tValue>=tMin ) : ( tValue>tMin );

	auto bMinOk = HAS_EQUAL_MIN ? ( tValue>=tMin ) : ( tValue>tMin );
	auto bMaxOk = HAS_EQUAL_MAX ? ( tValue<=tMax ) : ( tValue<tMax );

	return bMinOk && bMaxOk;
}


template<bool HAS_EQUAL_MIN, bool HAS_EQUAL_MAX, bool OPEN_LEFT = false, bool OPEN_RIGHT = false, typename T = SphAttr_t>
bool EvalBlockRangeAny ( T tMin1, T tMax1, T tMin2, T tMax2 )
{
	if_const ( OPEN_LEFT )
		return HAS_EQUAL_MAX ? ( tMin1<=tMax2 ) : ( tMin1<tMax2 );

	if_const ( OPEN_RIGHT )
		return HAS_EQUAL_MIN ? ( tMax1>=tMin2 ) : ( tMax1>tMin2 );

	auto bMinOk = HAS_EQUAL_MIN ? ( tMin1<=tMax2 ) : ( tMin1<tMax2 );
	auto bMaxOk = HAS_EQUAL_MAX ? ( tMax1>=tMin2 ) : ( tMax1>tMin2 );

	return bMinOk && bMaxOk;
}


template<bool HAS_EQUAL_MIN, bool HAS_EQUAL_MAX, typename T = SphAttr_t>
bool EvalBlockRangeAll ( T tMin1, T tMax1, T tMin2, T tMax2 )
{
	auto bMinOk = HAS_EQUAL_MIN ? ( tMin1>=tMin2 ) : ( tMin1>tMin2 );
	auto bMaxOk = HAS_EQUAL_MAX ? ( tMax1<=tMax2 ) : ( tMax1<tMax2 );

	return bMinOk && bMaxOk;
}


/// range
struct IFilter_Range: virtual ISphFilter
{
	SphAttr_t m_iMinValue;
	SphAttr_t m_iMaxValue;

	void SetRange ( SphAttr_t tMin, SphAttr_t tMax ) override
	{
		m_iMinValue = tMin;
		m_iMaxValue = tMax;
	}
};


/// filters

// attr

struct Filter_Values: public IFilter_Attr, IFilter_Values
{
	bool Eval ( const CSphMatch & tMatch ) const final
	{
		return EvalValues ( tMatch.GetAttr ( m_tLocator ) );
	}

	bool EvalBlock ( const DWORD * pMinDocinfo, const DWORD * pMaxDocinfo ) const final
	{
		if ( m_tLocator.m_bDynamic )
			return true; // ignore computed attributes

		SphAttr_t uBlockMin = sphGetRowAttr ( DOCINFO2ATTRS ( pMinDocinfo ), m_tLocator );
		SphAttr_t uBlockMax = sphGetRowAttr ( DOCINFO2ATTRS ( pMaxDocinfo ), m_tLocator );

		return EvalBlockValues ( uBlockMin, uBlockMax );
	}
};


struct Filter_SingleValue : public IFilter_Attr
{
	SphAttr_t m_RefValue;

#ifndef NDEBUG
	void SetValues ( const SphAttr_t * pStorage, int iCount ) final
#else
	virtual void SetValues ( const SphAttr_t * pStorage, int )
#endif
	{
		assert ( pStorage );
		assert ( iCount==1 );
		m_RefValue = (*pStorage);
	}

	bool Eval ( const CSphMatch & tMatch ) const override
	{
		return tMatch.GetAttr ( m_tLocator )==m_RefValue;
	}

	bool EvalBlock ( const DWORD * pMinDocinfo, const DWORD * pMaxDocinfo ) const final
	{
		if ( m_tLocator.m_bDynamic )
			return true; // ignore computed attributes

		SphAttr_t uBlockMin = sphGetRowAttr ( DOCINFO2ATTRS ( pMinDocinfo ), m_tLocator );
		SphAttr_t uBlockMax = sphGetRowAttr ( DOCINFO2ATTRS ( pMaxDocinfo ), m_tLocator );
		return ( uBlockMin<=m_RefValue && m_RefValue<=uBlockMax );
	}
};


struct Filter_SingleValueStatic32 : public Filter_SingleValue
{
	int m_iIndex;

	void SetLocator ( const CSphAttrLocator & tLoc ) final
	{
		assert ( tLoc.m_iBitCount==32 );
		assert ( ( tLoc.m_iBitOffset % 32 )==0 );
		assert ( !tLoc.m_bDynamic );
		m_tLocator = tLoc;
		m_iIndex = tLoc.m_iBitOffset / 32;
	}

	bool Eval ( const CSphMatch & tMatch ) const final
	{
		return tMatch.m_pStatic [ m_iIndex ]==m_RefValue;
	}
};


template < bool HAS_EQUAL_MIN, bool HAS_EQUAL_MAX, bool OPEN_LEFT, bool OPEN_RIGHT >
struct Filter_Range: public IFilter_Attr, IFilter_Range
{
	bool Eval ( const CSphMatch & tMatch ) const final
	{
		return EvalRange<HAS_EQUAL_MIN,HAS_EQUAL_MAX,OPEN_LEFT,OPEN_RIGHT> ( tMatch.GetAttr ( m_tLocator ), m_iMinValue, m_iMaxValue );
	}

	bool EvalBlock ( const DWORD * pMinDocinfo, const DWORD * pMaxDocinfo ) const final
	{
		if ( m_tLocator.m_bDynamic )
			return true; // ignore computed attributes

		auto uBlockMin = sphGetRowAttr ( DOCINFO2ATTRS ( pMinDocinfo ), m_tLocator );
		auto uBlockMax = sphGetRowAttr ( DOCINFO2ATTRS ( pMaxDocinfo ), m_tLocator );

		// not-reject
		return EvalBlockRangeAny<HAS_EQUAL_MIN,HAS_EQUAL_MAX,OPEN_LEFT,OPEN_RIGHT> ( uBlockMin, uBlockMax, m_iMinValue, m_iMaxValue );
	}
};

// float

template < bool HAS_EQUAL_MIN, bool HAS_EQUAL_MAX >
struct Filter_FloatRange : public IFilter_Attr
{
	float m_fMinValue;
	float m_fMaxValue;

	void SetRangeFloat ( float fMin, float fMax ) final
	{
		m_fMinValue = fMin;
		m_fMaxValue = fMax;
	}

	bool Eval ( const CSphMatch & tMatch ) const final
	{
		return EvalRange<HAS_EQUAL_MIN,HAS_EQUAL_MAX> ( tMatch.GetAttrFloat ( m_tLocator ), m_fMinValue, m_fMaxValue );
	}

	bool EvalBlock ( const DWORD * pMinDocinfo, const DWORD * pMaxDocinfo ) const final
	{
		if ( m_tLocator.m_bDynamic )
			return true; // ignore computed attributes

		float fBlockMin = sphDW2F ( (DWORD)sphGetRowAttr ( DOCINFO2ATTRS ( pMinDocinfo ), m_tLocator ) );
		float fBlockMax = sphDW2F ( (DWORD)sphGetRowAttr ( DOCINFO2ATTRS ( pMaxDocinfo ), m_tLocator ) );

		// not-reject
		return EvalBlockRangeAny<HAS_EQUAL_MIN,HAS_EQUAL_MAX> ( fBlockMin, fBlockMax, m_fMinValue, m_fMaxValue );
	}
};

// id

struct Filter_IdValues: public IFilter_Values
{
	bool Eval ( const CSphMatch & tMatch ) const final
	{
		return EvalValues ( tMatch.m_uDocID );
	}

	bool EvalBlock ( const DWORD * pMinDocinfo, const DWORD * pMaxDocinfo ) const final
	{
		const SphAttr_t uBlockMin = DOCINFO2ID ( pMinDocinfo );
		const SphAttr_t uBlockMax = DOCINFO2ID ( pMaxDocinfo );

		for ( int i = 0; i<m_iValueCount; ++i )
			if ( ( SphDocID_t ) GetValue ( i )>=( SphDocID_t ) uBlockMin
				&& ( SphDocID_t ) GetValue ( i )<=( SphDocID_t ) uBlockMax )
				return true;
		return false;
	}

	Filter_IdValues ()
	{
		m_bUsesAttrs = false;
	}
};

template < bool HAS_EQUAL_MIN, bool HAS_EQUAL_MAX >
struct Filter_IdRange: public IFilter_Range
{
	void SetRange ( SphAttr_t tMin, SphAttr_t tMax ) final
	{
		m_iMinValue = (SphDocID_t)Max ( (SphDocID_t)tMin, 0u );
		m_iMaxValue = tMax;
	}

	bool Eval ( const CSphMatch & tMatch ) const final
	{
		return EvalRange<HAS_EQUAL_MIN, HAS_EQUAL_MAX>( tMatch.m_uDocID, (SphDocID_t)m_iMinValue, (SphDocID_t)m_iMaxValue );
	}

	bool EvalBlock ( const DWORD * pMinDocinfo, const DWORD * pMaxDocinfo ) const final
	{
		const SphDocID_t uBlockMin = DOCINFO2ID ( pMinDocinfo );
		const SphDocID_t uBlockMax = DOCINFO2ID ( pMaxDocinfo );

		// not-reject
		return EvalBlockRangeAny<HAS_EQUAL_MIN,HAS_EQUAL_MAX> ( uBlockMin, uBlockMax, (SphDocID_t)m_iMinValue, (SphDocID_t)m_iMaxValue );
	}

	Filter_IdRange ()
	{
		m_bUsesAttrs = false;
	}
};

struct Filter_WeightValues: public IFilter_Values
{
	bool Eval ( const CSphMatch & tMatch ) const final
	{
		return EvalValues ( tMatch.m_iWeight );
	}

	Filter_WeightValues ()
	{
		m_bUsesAttrs = false;
	}
};

template < bool HAS_EQUAL_MIN, bool HAS_EQUAL_MAX >
struct Filter_WeightRange: public IFilter_Range
{
	virtual bool IsEarly () { return false; }
	bool Eval ( const CSphMatch & tMatch ) const final
	{
		return EvalRange<HAS_EQUAL_MIN, HAS_EQUAL_MAX> ( (SphAttr_t)tMatch.m_iWeight, m_iMinValue, m_iMaxValue );
	}

	Filter_WeightRange ()
	{
		m_bUsesAttrs = false;
	}
};

//////////////////////////////////////////////////////////////////////////
// MVA
//////////////////////////////////////////////////////////////////////////

struct IFilter_MVA: virtual IFilter_Attr
{
	const DWORD *	m_pMvaStorage = nullptr;
	bool			m_bArenaProhibit = false;

	void SetMVAStorage ( const DWORD * pMva, bool bArenaProhibit ) final
	{
		m_pMvaStorage = pMva;
		m_bArenaProhibit = bArenaProhibit;
	}

	inline bool LoadMVA ( const CSphMatch & tMatch, const DWORD ** pMva, const DWORD ** pMvaMax ) const
	{
		assert ( m_pMvaStorage );

		*pMva = tMatch.GetAttrMVA ( m_tLocator, m_pMvaStorage, m_bArenaProhibit );
		if ( !*pMva )
			return false;

		*pMvaMax = *pMva + (**pMva) + 1;
		(*pMva)++;
		return true;
	}
};


template < typename T >
struct Filter_MVAValues_Any : public IFilter_MVA, IFilter_Values
{
	bool Eval ( const CSphMatch & tMatch ) const final
	{
		const DWORD * pMva, * pMvaMax;
		if ( !LoadMVA ( tMatch, &pMva, &pMvaMax ) )
			return false;

		return MvaEval ( pMva, pMvaMax );
	}

	bool MvaEval ( const DWORD * pMva, const DWORD * pMvaMax ) const
	{
		const SphAttr_t * pFilter = m_pValues;
		const SphAttr_t * pFilterMax = pFilter + m_iValueCount;

		auto L = (const T *)pMva;
		auto R = ((const T *)pMvaMax)-1;
		for ( ; pFilter < pFilterMax; ++pFilter )
		{
			while ( L<=R )
			{
				const T * pVal = L + (R - L) / 2;
				if ( *pFilter > *pVal )
					L = pVal + 1;
				else if ( *pFilter < *pVal )
					R = pVal - 1;
				else
					return true;
			}
			R = (const T *)pMvaMax;
			--R;
		}
		return false;
	}
};


template < typename T >
struct Filter_MVAValues_All : public IFilter_MVA, IFilter_Values
{
	bool Eval ( const CSphMatch & tMatch ) const final
	{
		const DWORD * pMva, * pMvaMax;
		if ( !LoadMVA ( tMatch, &pMva, &pMvaMax ) )
			return false;

		return MvaEval ( pMva, pMvaMax );
	}

	bool MvaEval ( const DWORD * pMva, const DWORD * pMvaMax ) const
	{
		auto L = (const T *)pMva;
		auto R = (const T *)pMvaMax;

		for ( const T * pVal=L; pVal<R; ++pVal )
		{
			const SphAttr_t iCheck = *pVal;
			if ( !sphBinarySearch ( m_pValues, m_pValues + m_iValueCount - 1, iCheck ) )
				return false;
		}
		return true;
	}
};


static DWORD GetMvaValue ( const DWORD * pVal )
{
	return *pVal;
}

static int64_t GetMvaValue ( const int64_t * pVal )
{
	return MVA_UPSIZE ( (const DWORD *)pVal );
}

template < typename T, bool HAS_EQUAL_MIN, bool HAS_EQUAL_MAX >
struct Filter_MVARange_Any : public IFilter_MVA, IFilter_Range
{
	bool Eval ( const CSphMatch & tMatch ) const final
	{
		const DWORD * pMva, * pMvaMax;
		if ( !LoadMVA ( tMatch, &pMva, &pMvaMax ) )
			return false;

		return MvaEval ( pMva, pMvaMax );
	}

	bool MvaEval ( const DWORD * pMva, const DWORD * pMvaMax ) const
	{
		const T * pEnd = (const T *)pMvaMax;
		const T * L = (const T *)pMva;
		const T * R = pEnd - 1;

		while ( L<=R )
		{
			const T * pVal = L + (R - L) / 2;
			T iMva = GetMvaValue ( pVal );

			if ( m_iMinValue>iMva )
				L = pVal + 1;
			else if ( m_iMinValue<iMva )
				R = pVal - 1;
			else
				return ( HAS_EQUAL_MIN || pVal+1<pEnd );
		}
		if ( L==pEnd )
			return false;

		T iMvaL = GetMvaValue ( L );
		if_const ( HAS_EQUAL_MAX )
			return iMvaL<=m_iMaxValue;
		else
			return iMvaL<m_iMaxValue;
	}
};


template < typename T, bool HAS_EQUAL_MIN, bool HAS_EQUAL_MAX >
struct Filter_MVARange_All : public IFilter_MVA, IFilter_Range
{
	bool Eval ( const CSphMatch & tMatch ) const final
	{
		const DWORD * pMva, * pMvaMax;
		if ( !LoadMVA ( tMatch, &pMva, &pMvaMax ) )
			return false;

		return MvaEval ( pMva, pMvaMax );
	}

	bool MvaEval ( const DWORD * pMva, const DWORD * pMvaMax ) const
	{
		const T * L = (const T *)pMva;
		const T * pEnd = (const T *)pMvaMax;
		const T * R = pEnd - 1;

		return EvalBlockRangeAll<HAS_EQUAL_MIN,HAS_EQUAL_MAX> ( *L, *R, (T)m_iMinValue, (T)m_iMaxValue );
	}
};


SphStringCmp_fn CmpFn ( ESphCollation eCollation )
{
	switch ( eCollation )
	{
		case SPH_COLLATION_LIBC_CI:			return sphCollateLibcCI;
		case SPH_COLLATION_LIBC_CS:			return sphCollateLibcCS;
		case SPH_COLLATION_UTF8_GENERAL_CI: return sphCollateUtf8GeneralCI;
		default:							return sphCollateBinary;
	}
}

class IFilter_Str : public IFilter_Attr
{

protected:
	const BYTE * m_pStringBase = nullptr;
	StringSource_e m_eStrSource;

public:
	explicit IFilter_Str ( ESphAttr eType )
		: m_eStrSource ( eType==SPH_ATTR_STRING ? STRING_STATIC : STRING_DATAPTR )
	{}

	void SetStringStorage ( const BYTE * pStrings ) final
	{
		m_pStringBase = pStrings;
	}

protected:
	inline const BYTE * GetStr ( const CSphMatch& tMatch ) const
	{
		SphAttr_t uVal = tMatch.GetAttr ( m_tLocator );

		if ( !uVal )
			return ( const BYTE * ) "\0"; // 2 bytes, for packed strings

		if ( m_eStrSource==STRING_STATIC )
			return m_pStringBase + uVal;

		return ( const BYTE * ) uVal;
	}
};


class FilterString_c : public IFilter_Str
{
private:
	bool					m_bEq;

protected:
	CSphVector<BYTE>		m_dVal;
	SphStringCmp_fn			m_fnStrCmp;

public:
	FilterString_c ( ESphCollation eCollation, ESphAttr eType, bool bEq )
		: IFilter_Str ( eType )
		, m_bEq ( bEq )
		, m_fnStrCmp ( CmpFn ( eCollation ) )
	{}

	void SetRefString ( const CSphString * pRef, int iCount ) override
	{
		assert ( iCount<2 );
		const char * sVal = pRef ? pRef->cstr() : nullptr;
		int iLen = pRef ? pRef->Length() : 0;
		if ( m_eStrSource==STRING_STATIC )
		{
			// pack string as it is stored in index (.sps file)
			m_dVal.Resize ( iLen+4 );
			int iPacked = sphPackStrlen ( m_dVal.Begin(), iLen );
			memcpy ( m_dVal.Begin()+iPacked, sVal, iLen );
		} else
		{
			// pack string as it is stored in memory
			m_dVal.Resize ( sphCalcPackedLength ( iLen ) );
			sphPackPtrAttr ( m_dVal.Begin(), (const BYTE*)sVal, iLen );
		}
	}

	bool Eval ( const CSphMatch & tMatch ) const override
	{
		if ( m_eStrSource==STRING_STATIC && !m_pStringBase )
			return !m_bEq;

		bool bEq = m_fnStrCmp ( GetStr ( tMatch ), m_dVal.Begin (), m_eStrSource, 0, 0 )==0;
		return ( m_bEq==bEq );
	}
};


struct Filter_StringValues_c: FilterString_c
{
	CSphVector<int>			m_dOfs;

	Filter_StringValues_c ( ESphCollation eCollation, ESphAttr eType )
		: FilterString_c ( eCollation, eType, false )
	{}

	void SetRefString ( const CSphString * pRef, int iCount ) final
	{
		assert ( pRef );
		assert ( iCount>0 );

		int iOfs = 0;
		for ( int i=0; i<iCount; ++i )
		{
			const char * sRef = ( pRef + i )->cstr();
			int iLen = ( pRef + i )->Length();

			if ( m_eStrSource==STRING_STATIC )
			{
				m_dVal.Resize ( iOfs+iLen+4 );
				int iPacked = sphPackStrlen ( m_dVal.Begin() + iOfs, iLen );
				memcpy ( m_dVal.Begin() + iOfs + iPacked, sRef, iLen );
			} else
			{
				int iPackedLen = sphCalcPackedLength ( iLen );
				m_dVal.Resize ( iOfs+iPackedLen );
				sphPackPtrAttr ( m_dVal.Begin() + iOfs, (const BYTE*)sRef, iLen );
			}

			m_dOfs.Add ( iOfs );
			iOfs = m_dVal.GetLength();
		}
	}

	bool Eval ( const CSphMatch & tMatch ) const final
	{
		for ( int iOffs: m_dOfs )
			if ( m_fnStrCmp ( GetStr ( tMatch ), m_dVal.Begin() + iOffs, m_eStrSource, 0, 0 )==0 )
				return true;

		return false;
	}
};


struct Filter_StringTags_c : IFilter_Str
{
protected:
	CSphVector<uint64_t> m_dTags;
	mutable CSphVector<uint64_t> m_dMatchTags;

public:
	explicit Filter_StringTags_c ( ESphAttr eType )
		: IFilter_Str ( eType )
	{}

	void SetRefString ( const CSphString * pRef, int iCount ) final
	{
		assert ( pRef );
		assert ( iCount>0 );

		m_dTags.Resize ( iCount );

		for ( int i = 0; i<iCount; ++i )
			m_dTags[i] = sphFNV64 ( pRef[i].cstr ());

		m_dTags.Uniq();
	}

protected:
	inline void GetMatchTags ( const CSphMatch &tMatch ) const
	{
		SphAttr_t uVal = tMatch.GetAttr ( m_tLocator );

		const BYTE * pStr = nullptr;
		int iStrLen = -1;

		if ( m_eStrSource==STRING_STATIC )
			pStr = m_pStringBase + uVal;
		else
			pStr = ( const BYTE * ) uVal;

		UnpackString ( pStr, iStrLen, m_eStrSource );

		m_dMatchTags.Resize(0);
		sphSplitApply ( ( const char * ) pStr, iStrLen, [this] (const char* pTag, int iLen)
		{
			m_dMatchTags.Add ( sphFNV64 ( pTag, iLen, SPH_FNV64_SEED));
		} );
		m_dMatchTags.Uniq();
	}
};

struct Filter_StringTagsAny_c : Filter_StringTags_c
{
public:
	explicit Filter_StringTagsAny_c ( ESphAttr eType )
		: Filter_StringTags_c ( eType )
	{}

	bool Eval ( const CSphMatch &tMatch ) const final
	{
		GetMatchTags ( tMatch );

		auto pFilter = m_dMatchTags.begin ();
		auto pQueryTags = m_dTags.begin ();
		auto pFilterEnd = m_dMatchTags.end ();
		auto pTagsEnd = m_dTags.end ();

		while ( pFilter<pFilterEnd && pQueryTags<pTagsEnd )
		{
			if ( *pQueryTags<*pFilter )
				++pQueryTags;
			else if ( *pFilter<*pQueryTags )
				++pFilter;
			else if ( *pQueryTags==*pFilter )
				return true;
		}
		return false;
	}
};

struct Filter_StringTagsAll_c : Filter_StringTags_c
{
public:
	explicit Filter_StringTagsAll_c ( ESphAttr eType )
		: Filter_StringTags_c ( eType )
	{}

	bool Eval ( const CSphMatch &tMatch ) const final
	{
		GetMatchTags ( tMatch );

		auto pFilter = m_dMatchTags.begin ();
		auto pQueryTags = m_dTags.begin ();
		auto pFilterEnd = m_dMatchTags.end ();
		auto pTagsEnd = m_dTags.end ();
		int iExpectedTags = m_dTags.GetLength();

		while ( pFilter<pFilterEnd && pQueryTags<pTagsEnd && iExpectedTags>0 )
		{
			if ( *pQueryTags<*pFilter )
				++pQueryTags;
			else if ( *pFilter<*pQueryTags )
				++pFilter;
			else if ( *pQueryTags==*pFilter )
			{
				--iExpectedTags;
				++pQueryTags;
				++pFilter;
			}
		}
		return !iExpectedTags;
	}
};


struct Filter_And2 : public ISphFilter
{
	ISphFilter * m_pArg1;
	ISphFilter * m_pArg2;

	explicit Filter_And2 ( ISphFilter * pArg1, ISphFilter * pArg2, bool bUsesAttrs )
		: m_pArg1 ( pArg1 )
		, m_pArg2 ( pArg2 )
	{
		m_bUsesAttrs = bUsesAttrs;
	}

	~Filter_And2 () final
	{
		SafeDelete ( m_pArg1 );
		SafeDelete ( m_pArg2 );
	}

	bool Eval ( const CSphMatch & tMatch ) const final
	{
		return m_pArg1->Eval ( tMatch ) && m_pArg2->Eval ( tMatch );
	}

	bool EvalBlock ( const DWORD * pMin, const DWORD * pMax ) const final
	{
		return m_pArg1->EvalBlock ( pMin, pMax ) && m_pArg2->EvalBlock ( pMin, pMax );
	}

	ISphFilter * Join ( ISphFilter * pFilter ) final
	{
		m_pArg2 = new Filter_And2 ( m_pArg2, pFilter, m_bUsesAttrs );
		return this;
	}

	void SetMVAStorage ( const DWORD * pMva, bool bArenaProhibit ) final
	{
		m_pArg1->SetMVAStorage ( pMva, bArenaProhibit );
		m_pArg2->SetMVAStorage ( pMva, bArenaProhibit );
	}

	void SetStringStorage ( const BYTE * pStrings ) final
	{
		m_pArg1->SetStringStorage ( pStrings );
		m_pArg2->SetStringStorage ( pStrings );
	}
};


struct Filter_And3 : public ISphFilter
{
	ISphFilter * m_pArg1;
	ISphFilter * m_pArg2;
	ISphFilter * m_pArg3;

	explicit Filter_And3 ( ISphFilter * pArg1, ISphFilter * pArg2, ISphFilter * pArg3, bool bUsesAttrs )
		: m_pArg1 ( pArg1 )
		, m_pArg2 ( pArg2 )
		, m_pArg3 ( pArg3 )
	{
		m_bUsesAttrs = bUsesAttrs;
	}

	~Filter_And3 () final
	{
		SafeDelete ( m_pArg1 );
		SafeDelete ( m_pArg2 );
		SafeDelete ( m_pArg3 );
	}

	bool Eval ( const CSphMatch & tMatch ) const final
	{
		return m_pArg1->Eval ( tMatch ) && m_pArg2->Eval ( tMatch ) && m_pArg3->Eval ( tMatch );
	}

	bool EvalBlock ( const DWORD * pMin, const DWORD * pMax ) const final
	{
		return m_pArg1->EvalBlock ( pMin, pMax ) && m_pArg2->EvalBlock ( pMin, pMax ) && m_pArg3->EvalBlock ( pMin, pMax );
	}

	ISphFilter * Join ( ISphFilter * pFilter ) final
	{
		m_pArg3 = new Filter_And2 ( m_pArg3, pFilter, m_bUsesAttrs );
		return this;
	}

	void SetMVAStorage ( const DWORD * pMva, bool bArenaProhibit ) final
	{
		m_pArg1->SetMVAStorage ( pMva, bArenaProhibit );
		m_pArg2->SetMVAStorage ( pMva, bArenaProhibit );
		m_pArg3->SetMVAStorage ( pMva, bArenaProhibit );
	}

	void SetStringStorage ( const BYTE * pStrings ) final
	{
		m_pArg1->SetStringStorage ( pStrings );
		m_pArg2->SetStringStorage ( pStrings );
		m_pArg3->SetStringStorage ( pStrings );
	}
};


struct Filter_And: public ISphFilter
{
	CSphVector<ISphFilter *> m_dFilters;

	~Filter_And () final
	{
		for ( auto &pFilter : m_dFilters )
			SafeDelete ( pFilter );
	}

	void Add ( ISphFilter * pFilter )
	{
		m_dFilters.Add ( pFilter );
		m_bUsesAttrs |= pFilter->UsesAttrs();
	}

	bool Eval ( const CSphMatch & tMatch ) const final
	{
		for ( auto pFilter: m_dFilters )
			if ( !pFilter->Eval ( tMatch ) )
				return false;
		return true;
	}

	bool EvalBlock ( const DWORD * pMinDocinfo, const DWORD * pMaxDocinfo ) const final
	{
		for ( auto pFilter : m_dFilters )
			if ( !pFilter->EvalBlock ( pMinDocinfo, pMaxDocinfo ) )
				return false;
		return true;
	}

	ISphFilter * Join ( ISphFilter * pFilter ) final
	{
		Add ( pFilter );
		return this;
	}

	Filter_And ()
	{
		m_bUsesAttrs = false;
	}


	void SetMVAStorage ( const DWORD * pMva, bool bArenaProhibit ) final
	{
		for ( auto &pFilter: m_dFilters )
			pFilter->SetMVAStorage ( pMva, bArenaProhibit );
	}

	void SetStringStorage ( const BYTE * pStrings ) final
	{
		for ( auto &pFilter : m_dFilters )
			pFilter->SetStringStorage ( pStrings );
	}

	ISphFilter * Optimize() final
	{
		if ( m_dFilters.GetLength()==2 )
		{
			ISphFilter * pOpt = new Filter_And2 ( m_dFilters[0], m_dFilters[1], m_bUsesAttrs );
			m_dFilters.Reset();
			delete this;
			return pOpt;
		}
		if ( m_dFilters.GetLength()==3 )
		{
			ISphFilter * pOpt = new Filter_And3 ( m_dFilters[0], m_dFilters[1], m_dFilters[2], m_bUsesAttrs );
			m_dFilters.Reset();
			delete this;
			return pOpt;
		}
		return this;
	}
};


struct Filter_Or: public ISphFilter
{
	ISphFilter * m_pLeft;
	ISphFilter * m_pRight;

	Filter_Or ( ISphFilter * pLeft, ISphFilter * pRight )
		: m_pLeft ( pLeft )
		, m_pRight ( pRight )
	{
		m_bUsesAttrs |= pLeft->UsesAttrs();
		m_bUsesAttrs |= pRight->UsesAttrs();
	}

	~Filter_Or () final
	{
		SafeDelete ( m_pLeft );
		SafeDelete ( m_pRight );
	}

	bool Eval ( const CSphMatch & tMatch ) const final
	{
		return ( m_pLeft->Eval ( tMatch ) || m_pRight->Eval ( tMatch ) );
	}

	bool EvalBlock ( const DWORD * pMinDocinfo, const DWORD * pMaxDocinfo ) const final
	{
		return ( m_pLeft->EvalBlock ( pMinDocinfo, pMaxDocinfo ) || m_pRight->EvalBlock ( pMinDocinfo, pMaxDocinfo ) );
	}

	void SetMVAStorage ( const DWORD * pMva, bool bArenaProhibit ) final
	{
		m_pLeft->SetMVAStorage ( pMva, bArenaProhibit );
		m_pRight->SetMVAStorage ( pMva, bArenaProhibit );
	}

	void SetStringStorage ( const BYTE * pStrings ) final
	{
		m_pLeft->SetStringStorage ( pStrings );
		m_pRight->SetStringStorage ( pStrings );
	}

	ISphFilter * Optimize() final
	{
		m_pLeft->Optimize();
		m_pRight->Optimize();
		return this;
	}
};


// not

struct Filter_Not: public ISphFilter
{
	ISphFilter * m_pFilter;

	explicit Filter_Not ( ISphFilter * pFilter )
		: m_pFilter ( pFilter )
	{
		assert ( pFilter );
		m_bUsesAttrs = pFilter->UsesAttrs();
	}

	~Filter_Not () final
	{
		SafeDelete ( m_pFilter );
	}

	bool Eval ( const CSphMatch & tMatch ) const final
	{
		return !m_pFilter->Eval ( tMatch );
	}

	bool EvalBlock ( const DWORD *, const DWORD * ) const final
	{
		// if block passes through the filter we can't just negate the
		// result since it's imprecise at this point
		return true;
	}

	void SetMVAStorage ( const DWORD * pMva, bool bArenaProhibit ) final
	{
		m_pFilter->SetMVAStorage ( pMva, bArenaProhibit );
	}

	void SetStringStorage ( const BYTE * pStrings ) final
	{
		m_pFilter->SetStringStorage ( pStrings );
	}
};

/// impl

ISphFilter * ISphFilter::Join ( ISphFilter * pFilter )
{
	auto pAnd = new Filter_And();

	pAnd->Add ( this );
	pAnd->Add ( pFilter );

	return pAnd;
}

/// helper functions

static inline ISphFilter * ReportError ( CSphString & sError, const char * sMessage, ESphFilter eFilterType )
{
	CSphString sFilterName;
	switch ( eFilterType )
	{
		case SPH_FILTER_VALUES:			sFilterName = "intvalues"; break;
		case SPH_FILTER_RANGE:			sFilterName = "intrange"; break;
		case SPH_FILTER_FLOATRANGE:		sFilterName = "floatrange"; break;
		case SPH_FILTER_STRING:			sFilterName = "string"; break;
		case SPH_FILTER_NULL:			sFilterName = "null"; break;
		default:						sFilterName.SetSprintf ( "(filter-type-%d)", eFilterType ); break;
	}

	sError.SetSprintf ( sMessage, sFilterName.cstr() );
	return nullptr;
}


#define CREATE_RANGE_FILTER(FILTER,SETTINGS) \
{ \
	if ( SETTINGS.m_bHasEqualMin ) \
	{ \
		if ( SETTINGS.m_bHasEqualMax ) \
			return new FILTER<true,true>; \
		else \
			return new FILTER<true,false>; \
	} else \
	{ \
		if ( SETTINGS.m_bHasEqualMax ) \
			return new FILTER<false,true>; \
		else \
			return new FILTER<false,false>; \
	} \
}


#define CREATE_RANGE_FILTER_WITH_OPEN(FILTER,SETTINGS) \
{ \
	if ( SETTINGS.m_bOpenLeft ) \
	{ \
		if ( SETTINGS.m_bHasEqualMax ) \
			return new FILTER<true,true,true,false>; \
		else \
			return new FILTER<true,false,true,false>; \
	} else if ( SETTINGS.m_bOpenRight ) \
	{ \
		if ( SETTINGS.m_bHasEqualMin ) \
			return new FILTER<true,true,false,true>; \
		else \
			return new FILTER<false,true,false,true>; \
	} \
	assert ( !SETTINGS.m_bOpenLeft && !SETTINGS.m_bOpenRight ); \
	if ( SETTINGS.m_bHasEqualMin ) \
	{ \
		if ( SETTINGS.m_bHasEqualMax ) \
			return new FILTER<true,true,false,false>; \
		else \
			return new FILTER<true,false,false,false>; \
	} else \
	{ \
		if ( SETTINGS.m_bHasEqualMax ) \
			return new FILTER<false,true,false,false>; \
		else \
			return new FILTER<false,false,false,false>; \
	} \
}


#define CREATE_MVA_RANGE_FILTER(FILTER,T,SETTINGS) \
{ \
	if ( SETTINGS.m_bHasEqualMin ) \
	{ \
		if ( SETTINGS.m_bHasEqualMax ) \
			return new FILTER<T,true,true>; \
		else \
			return new FILTER<T,true,false>; \
	} else \
	{ \
		if ( SETTINGS.m_bHasEqualMax ) \
			return new FILTER<T,false,true>; \
		else \
			return new FILTER<T,false,false>; \
	} \
}


#define CREATE_EXPR_RANGE_FILTER(FILTER,EXPR,bHasEqualMin,bHasEqualMax) \
{ \
	if ( bHasEqualMin ) \
	{ \
		if ( bHasEqualMax ) \
			return new FILTER<true,true>(EXPR); \
		else \
			return new FILTER<true,false>(EXPR); \
	} else \
	{ \
		if ( bHasEqualMax ) \
			return new FILTER<false,true>(EXPR); \
		else \
			return new FILTER<false,false>(EXPR); \
	} \
}


static ISphFilter * CreateSpecialFilter ( const CSphString & sName, const CSphFilterSettings & tSettings, CSphString & sError )
{
	if ( sName=="@id" )
	{
		switch ( tSettings.m_eType )
		{
		case SPH_FILTER_VALUES:	return new Filter_IdValues;
		case SPH_FILTER_RANGE:	CREATE_RANGE_FILTER(Filter_IdRange,tSettings);
		default:
			return ReportError ( sError, "unsupported filter type '%s' on @id", tSettings.m_eType );
		}
	} else if ( sName=="@weight" )
	{
		switch ( tSettings.m_eType )
		{
		case SPH_FILTER_VALUES:	return new Filter_WeightValues;
		case SPH_FILTER_RANGE:	CREATE_RANGE_FILTER ( Filter_WeightRange, tSettings );
		default:
			return ReportError ( sError, "unsupported filter type '%s' on @weight", tSettings.m_eType );
		}
	}

	return nullptr;
}


static ISphFilter * CreateFilter ( const CSphFilterSettings & tSettings, ESphFilter eFilterType,
	ESphAttr eAttrType, const CSphAttrLocator & tLoc, ESphCollation eCollation,
	CSphString & sError, CSphString & sWarning )
{
	// MVA
	if ( eAttrType==SPH_ATTR_UINT32SET || eAttrType==SPH_ATTR_INT64SET )
	{
		if (!( eFilterType==SPH_FILTER_VALUES || eFilterType==SPH_FILTER_RANGE ))
			return ReportError ( sError, "unsupported filter type '%s' on MVA column", eFilterType );

		if ( tSettings.m_eMvaFunc==SPH_MVAFUNC_NONE )
			sWarning.SetSprintf ( "suggest an explicit ANY()/ALL() around a filter on MVA column" );

		bool bWide = ( eAttrType==SPH_ATTR_INT64SET );
		bool bRange = ( eFilterType==SPH_FILTER_RANGE );
		bool bAll = ( tSettings.m_eMvaFunc==SPH_MVAFUNC_ALL );
		int iIndex = bWide*4 + bRange*2 + bAll;

		switch ( iIndex )
		{
			case 0:	return new Filter_MVAValues_Any<DWORD>();
			case 1:	return new Filter_MVAValues_All<DWORD>();

			case 2:	CREATE_MVA_RANGE_FILTER ( Filter_MVARange_Any, DWORD, tSettings );
			case 3:	CREATE_MVA_RANGE_FILTER ( Filter_MVARange_All, DWORD, tSettings );

			case 4:	return new Filter_MVAValues_Any<int64_t>();
			case 5:	return new Filter_MVAValues_All<int64_t>();

			case 6:	CREATE_MVA_RANGE_FILTER ( Filter_MVARange_Any, int64_t, tSettings );
			case 7:	CREATE_MVA_RANGE_FILTER ( Filter_MVARange_All, int64_t, tSettings );
			default:
				assert (false && "UB in CreateFilter");
		}
	}

	// float
	if ( eAttrType==SPH_ATTR_FLOAT )
	{
		if ( eFilterType==SPH_FILTER_FLOATRANGE || eFilterType==SPH_FILTER_RANGE )
			CREATE_RANGE_FILTER ( Filter_FloatRange, tSettings );

		return ReportError ( sError, "unsupported filter type '%s' on float column", eFilterType );
	}

	if ( eAttrType==SPH_ATTR_STRING || eAttrType==SPH_ATTR_STRINGPTR )
	{
		if ( eFilterType==SPH_FILTER_STRING_LIST )
		{
			if ( tSettings.m_eMvaFunc==SPH_MVAFUNC_NONE )
				return new Filter_StringValues_c ( eCollation, eAttrType );
			else if ( tSettings.m_eMvaFunc==SPH_MVAFUNC_ANY )
				return new Filter_StringTagsAny_c ( eAttrType );
			else if ( tSettings.m_eMvaFunc==SPH_MVAFUNC_ALL )
				return new Filter_StringTagsAll_c ( eAttrType );
		}

		else
			return new FilterString_c ( eCollation, eAttrType, tSettings.m_bHasEqualMin || tSettings.m_bHasEqualMax );
	}

	// non-float, non-MVA
	switch ( eFilterType )
	{
		case SPH_FILTER_VALUES:
			if ( tSettings.GetNumValues()==1 && ( eAttrType==SPH_ATTR_INTEGER || eAttrType==SPH_ATTR_BIGINT || eAttrType==SPH_ATTR_TOKENCOUNT ) )
			{
				if ( ( eAttrType==SPH_ATTR_INTEGER || eAttrType==SPH_ATTR_TOKENCOUNT ) && !tLoc.m_bDynamic && tLoc.m_iBitCount==32 && ( tLoc.m_iBitOffset % 32 )==0 )
					return new Filter_SingleValueStatic32();
				else
					return new Filter_SingleValue();
			} else
				return new Filter_Values();

		case SPH_FILTER_RANGE:	CREATE_RANGE_FILTER_WITH_OPEN ( Filter_Range, tSettings );
		default:				return ReportError ( sError, "unsupported filter type '%s' on int column", eFilterType );
	}
}

//////////////////////////////////////////////////////////////////////////
// EXPRESSION STUFF
//////////////////////////////////////////////////////////////////////////

template<typename BASE>
class ExprFilter_c : public BASE
{
protected:
	const BYTE *				m_pStrings = nullptr;
	CSphRefcountedPtr<ISphExpr>	m_pExpr;

public:
	explicit ExprFilter_c ( ISphExpr * pExpr )
		: m_pExpr ( pExpr )
	{
		SafeAddRef ( pExpr );
	}

	void SetStringStorage ( const BYTE * pStrings ) final
	{
		m_pStrings = pStrings;
		if ( m_pExpr )
			m_pExpr->Command ( SPH_EXPR_SET_STRING_POOL, (void*)pStrings );
	}
};


template < bool HAS_EQUAL_MIN, bool HAS_EQUAL_MAX >
class ExprFilterFloatRange_c : public ExprFilter_c<IFilter_Range>
{
public:
	explicit ExprFilterFloatRange_c ( ISphExpr * pExpr )
		: ExprFilter_c<IFilter_Range> ( pExpr )
	{}

	float m_fMinValue = 0.0f;
	float m_fMaxValue = 0.0f;

	void SetRangeFloat ( float fMin, float fMax ) override
	{
		m_fMinValue = fMin;
		m_fMaxValue = fMax;
	}

	bool Eval ( const CSphMatch & tMatch ) const final
	{
		return EvalRange<HAS_EQUAL_MIN, HAS_EQUAL_MAX,false,false,float> ( m_pExpr->Eval ( tMatch ), m_fMinValue, m_fMaxValue );
	}
};


template < bool HAS_EQUAL_MIN, bool HAS_EQUAL_MAX >
class ExprFilterRange_c : public ExprFilter_c<IFilter_Range>
{
public:
	explicit ExprFilterRange_c ( ISphExpr * pExpr )
		: ExprFilter_c<IFilter_Range> ( pExpr )
	{}

	bool Eval ( const CSphMatch & tMatch ) const final
	{
		return EvalRange<HAS_EQUAL_MIN, HAS_EQUAL_MAX> ( m_pExpr->Int64Eval(tMatch), m_iMinValue, m_iMaxValue );
	}
};


class ExprFilterValues_c : public ExprFilter_c<IFilter_Values>
{
public:
	explicit ExprFilterValues_c ( ISphExpr * pExpr )
		: ExprFilter_c<IFilter_Values> ( pExpr )
	{}

	bool Eval ( const CSphMatch & tMatch ) const final
	{
		assert ( this->m_pExpr );
		return EvalValues ( m_pExpr->Int64Eval ( tMatch ) );
	}
};


class ExprFilterString_c : public ExprFilter_c<ISphFilter>
{
protected:
	CSphString				m_sVal;
	int						m_iValLength = 0;
	SphStringCmp_fn			m_fnStrCmp;
	bool					m_bEq;

public:
	explicit ExprFilterString_c ( ISphExpr * pExpr, ESphCollation eCollation, bool bEq )
		: ExprFilter_c<ISphFilter> ( pExpr )
		, m_fnStrCmp ( CmpFn ( eCollation ) )
		, m_bEq ( bEq )
	{}

	void SetRefString ( const CSphString * pRef, int iCount ) final
	{
		assert ( iCount<2 );
		if ( pRef )
		{
			m_sVal = *pRef;
			m_iValLength = m_sVal.Length();
		}
	}

	bool Eval ( const CSphMatch & tMatch ) const final
	{
		const BYTE * pVal = nullptr;
		int iLen = m_pExpr->StringEval ( tMatch, &pVal );
		bool bEq = m_fnStrCmp ( pVal, (const BYTE*)m_sVal.cstr(), STRING_PLAIN, iLen, m_iValLength )==0;
		return ( m_bEq==bEq );
	}
};


class ExprFilterNull_c : public ExprFilter_c<ISphFilter>
{
protected:
	CSphAttrLocator m_tLoc;
	const bool m_bEquals;
	const bool m_bCheckOnlyKey;

public:
	ExprFilterNull_c ( ISphExpr * pExpr, bool bEquals, bool bCheckOnlyKey )
		: ExprFilter_c<ISphFilter> ( pExpr )
		, m_bEquals ( bEquals )
		, m_bCheckOnlyKey ( bCheckOnlyKey )
	{}

	void SetLocator ( const CSphAttrLocator & tLocator ) final
	{
		m_tLoc = tLocator;
	}

	ESphJsonType GetKey ( const CSphMatch & tMatch ) const
	{
		if ( !m_pStrings || !m_pExpr )
			return JSON_EOF;
		uint64_t uValue = m_pExpr->Int64Eval ( tMatch );
		if ( uValue==0 ) // either no data or invalid path
			return JSON_EOF;

		ESphJsonType eRes = (ESphJsonType)( uValue >> 32 );
		return eRes;
	}

	bool Eval ( const CSphMatch & tMatch ) const final
	{
		assert ( m_pStrings );

		if ( !m_pExpr ) // regular attribute? return true if blob size is null
		{
			const BYTE * pStr = nullptr;
			auto uOffset = (DWORD) tMatch.GetAttr ( m_tLoc );
			if ( uOffset )
				sphUnpackStr ( m_pStrings+uOffset, &pStr );
			return m_bEquals ^ ( pStr!=nullptr );
		}

		// possibly json
		ESphJsonType eRes = GetKey ( tMatch );
		if ( m_bCheckOnlyKey )
			return ( eRes!=JSON_EOF );

		return m_bEquals ^ ( eRes!=JSON_EOF && eRes!=JSON_NULL );
	}
};

// TODO: implement expression -> filter tree optimization to extract filters from general expression and got rid of all filters wo block-level optimizer
// wrapper for whole expression that evaluates to 0 and 1 on filtering
class ExprFilterProxy_c : public ExprFilter_c<ISphFilter>
{
protected:
	ESphAttr m_eAttrType = SPH_ATTR_NONE;

public:
	ExprFilterProxy_c ( ISphExpr * pExpr, ESphAttr eAttrType )
		: ExprFilter_c<ISphFilter> ( pExpr )
		, m_eAttrType ( eAttrType )
	{}

	bool Eval ( const CSphMatch & tMatch ) const final
	{
		switch ( m_eAttrType )
		{
			case SPH_ATTR_INTEGER:
				return ( m_pExpr->IntEval ( tMatch )>0 );

			case SPH_ATTR_BIGINT:
			case SPH_ATTR_JSON_FIELD:
				return ( m_pExpr->Int64Eval ( tMatch )>0 );

			case SPH_ATTR_INT64SET:
			case SPH_ATTR_UINT32SET:
				return ( m_pExpr->IntEval ( tMatch )>0 );

			default:
				return ( m_pExpr->Eval ( tMatch )>0.0f );
		}
	}
};



static ISphFilter * CreateFilterExpr ( ISphExpr * _pExpr, const CSphFilterSettings & tSettings, CSphString & sError, ESphCollation eCollation, ESphAttr eAttrType )
{
	// autoconvert all json types except SPH_FILTER_NULL, it needs more info
	bool bAutoConvert = ( eAttrType==SPH_ATTR_JSON || eAttrType==SPH_ATTR_JSON_FIELD )
		&& tSettings.m_eType!=SPH_FILTER_NULL;
	CSphRefcountedPtr<ISphExpr> pExpr { _pExpr };
	SafeAddRef ( _pExpr );
	if ( bAutoConvert )
		pExpr = sphJsonFieldConv ( pExpr );

	switch ( tSettings.m_eType )
	{
		case SPH_FILTER_VALUES:			return new ExprFilterValues_c ( pExpr );
		case SPH_FILTER_FLOATRANGE:		CREATE_EXPR_RANGE_FILTER ( ExprFilterFloatRange_c, pExpr, tSettings.m_bHasEqualMin, tSettings.m_bHasEqualMax );
		case SPH_FILTER_RANGE:			CREATE_EXPR_RANGE_FILTER ( ExprFilterRange_c, pExpr, tSettings.m_bHasEqualMin, tSettings.m_bHasEqualMax );
		case SPH_FILTER_STRING:			return new ExprFilterString_c ( pExpr, eCollation, tSettings.m_bHasEqualMin || tSettings.m_bHasEqualMax );
		case SPH_FILTER_NULL:			return new ExprFilterNull_c ( pExpr, tSettings.m_bIsNull, false );
		case SPH_FILTER_EXPRESSION:
		{
			if ( eAttrType!=SPH_ATTR_INTEGER && eAttrType!=SPH_ATTR_BIGINT && eAttrType!=SPH_ATTR_JSON_FIELD && eAttrType!=SPH_ATTR_FLOAT
				&& eAttrType!=SPH_ATTR_INT64SET && eAttrType!=SPH_ATTR_UINT32SET )
			{
				sError = "filter expression must evaluate to integer or float";
				return nullptr;
			} else
			{
				return new ExprFilterProxy_c ( pExpr, eAttrType );
			}
		}
		default:
			sError = "this filter type on expressions is not implemented yet";
			return nullptr;
	}
}

//////////////////////////////////////////////////////////////////////////
// KillList STUFF
//////////////////////////////////////////////////////////////////////////

static int g_iKillMerge = 128;

struct Filter_KillList : public ISphFilter
{
	KillListVector			m_dExt;
	CSphVector<SphDocID_t>	m_dMerged;

	explicit Filter_KillList ( const KillListVector & dKillList )
	{
		m_bUsesAttrs = false;
		int iMerged = 0;
		if ( dKillList.GetLength()>1 )
		{
			for ( const auto & dExt: dKillList )
				if ( dExt.m_iLen<=g_iKillMerge )
					iMerged += dExt.m_iLen;

			if ( iMerged>0 )
			{
				m_dMerged.Reserve ( iMerged );
				for ( const auto &dExt: dKillList )
				{
					if ( dExt.m_iLen>g_iKillMerge )
						m_dExt.Add ( dExt );
					else
						m_dMerged.Append ( dExt.m_pBegin, dExt.m_iLen );
				}
				m_dMerged.Uniq ();
			}
		};
		if ( m_dMerged.IsEmpty() )
			m_dExt = dKillList;
	}

	bool Eval ( const CSphMatch & tMatch ) const final
	{
		if ( !m_dMerged.GetLength() && !m_dExt.GetLength() )
			return true;

		if ( m_dMerged.BinarySearch ( tMatch.m_uDocID )!=nullptr )
			return false;

		for ( const auto &dExt: m_dExt )
			if ( sphBinarySearch ( dExt.m_pBegin, dExt.m_pBegin + dExt.m_iLen - 1, tMatch.m_uDocID )!=nullptr )
				return false;

		return true;
	}
};


//////////////////////////////////////////////////////////////////////////
// PUBLIC FACING INTERFACE
//////////////////////////////////////////////////////////////////////////

static ISphFilter * CreateFilter ( const CSphFilterSettings & tSettings, const CSphString & sAttrName, const ISphSchema & tSchema, const DWORD * pMvaPool, const BYTE * pStrings,
	CSphString & sError, CSphString & sWarning, bool bHaving, ESphCollation eCollation, bool bArenaProhibit )
{
	ISphFilter * pFilter = nullptr;
	const CSphColumnInfo * pAttr = nullptr;

	// try to create a filter on a special attribute
	if ( sAttrName.Begins("@") && !bHaving && ( sAttrName=="@groupby" || sAttrName=="@count" || sAttrName=="@distinct" ) )
	{
		sError.SetSprintf ( "unsupported filter column '%s'", sAttrName.cstr() );
		return nullptr;
	}

	if ( sAttrName.Begins("@") )
	{
		pFilter = CreateSpecialFilter ( sAttrName, tSettings, sError );
		if ( !pFilter && !sError.IsEmpty() )
			return nullptr;
	}

	// try to lookup a regular attribute
	ESphFilter eType = tSettings.m_eType;
	float fMin = tSettings.m_fMinValue;
	float fMax = tSettings.m_fMaxValue;

	ESphAttr eAttrType = SPH_ATTR_NONE;
	CSphRefcountedPtr<ISphExpr> pExpr;

	if ( !pFilter )
	{
		const int iAttr = ( tSettings.m_eType!=SPH_FILTER_EXPRESSION ? tSchema.GetAttrIndex ( sAttrName.cstr() ) : -1 );
		if ( iAttr<0 || tSettings.m_eType==SPH_FILTER_EXPRESSION )
		{
			// try expression
			pExpr = sphExprParse ( sAttrName.cstr(), tSchema, &eAttrType, nullptr, sError, nullptr, eCollation );
			if ( pExpr )
			{
				pFilter = CreateFilterExpr ( pExpr, tSettings, sError, eCollation, eAttrType );

			} else
			{
				sError.SetSprintf ( "no such filter attribute '%s'", sAttrName.cstr() );
				return nullptr;
			}

		} else
		{
			pAttr = &tSchema.GetAttr(iAttr);
			if ( !bHaving && pAttr->m_eAggrFunc!=SPH_AGGR_NONE )
			{
				sError.SetSprintf ( "unsupported filter '%s' on aggregate column", sAttrName.cstr() );
				return nullptr;
			}

			if ( pAttr->m_eAttrType==SPH_ATTR_JSON || pAttr->m_eAttrType==SPH_ATTR_JSON_FIELD )
			{
				pExpr = pAttr->m_pExpr;
				eAttrType = pAttr->m_eAttrType;
				pFilter = CreateFilterExpr ( pExpr, tSettings, sError, eCollation, pAttr->m_eAttrType );

			} else
			{
				// fixup "fltcol=intval" conditions
				if ( pAttr->m_eAttrType==SPH_ATTR_FLOAT && tSettings.m_eType==SPH_FILTER_VALUES && tSettings.GetNumValues()==1 )
				{
					eType = SPH_FILTER_FLOATRANGE;
					fMin = fMax = (float)tSettings.GetValue(0);
				}

				pFilter = CreateFilter ( tSettings, eType, pAttr->m_eAttrType, pAttr->m_tLocator, eCollation, sError, sWarning );
			}
		}
	}

	// fill filter's properties
	if ( pFilter )
	{
		if ( pAttr )
			pFilter->SetLocator ( pAttr->m_tLocator );

		pFilter->SetMVAStorage ( pMvaPool, bArenaProhibit );
		pFilter->SetStringStorage ( pStrings );

		pFilter->SetRange ( tSettings.m_iMinValue, tSettings.m_iMaxValue );
		if ( eType==SPH_FILTER_FLOATRANGE )
			pFilter->SetRangeFloat ( fMin, fMax );
		else
			pFilter->SetRangeFloat ( (float)tSettings.m_iMinValue, (float)tSettings.m_iMaxValue );

		pFilter->SetRefString ( tSettings.m_dStrings.Begin(), tSettings.m_dStrings.GetLength() );
		if ( tSettings.GetNumValues() > 0 )
		{
			pFilter->SetValues ( tSettings.GetValueArray(), tSettings.GetNumValues() );
#ifndef NDEBUG
			// check that the values are actually sorted
			const SphAttr_t * pValues = tSettings.GetValueArray();
			int iValues = tSettings.GetNumValues ();
			for ( int i=1; i<iValues; ++i )
				assert ( pValues[i]>=pValues[i-1] );
#endif
		}

		if ( tSettings.m_bExclude )
			pFilter = new Filter_Not ( pFilter );


		// filter for json || json.field needs to check that key exists
		if ( ( eAttrType==SPH_ATTR_JSON || eAttrType==SPH_ATTR_JSON_FIELD ) && tSettings.m_eType!=SPH_FILTER_NULL && pFilter && pExpr )
		{
			ISphFilter * pNotNull = new ExprFilterNull_c ( pExpr, false, true );
			pNotNull->SetStringStorage ( pStrings );
			pFilter = new Filter_And2 ( pFilter, pNotNull, true );
		}
	}

	return pFilter;
}


ISphFilter * sphCreateFilter ( const CSphFilterSettings &tSettings, const CreateFilterContext_t &tCtx, CSphString &sError, CSphString &sWarning )
{
	return CreateFilter ( tSettings, tSettings.m_sAttrName, *tCtx.m_pSchema,
		tCtx.m_pMvaPool, tCtx.m_pStrings, sError, sWarning, false,
		tCtx.m_eCollation, tCtx.m_bArenaProhibit );
}

ISphFilter * sphCreateAggrFilter ( const CSphFilterSettings * pSettings, const CSphString & sAttrName, const ISphSchema & tSchema, CSphString & sError )
{
	assert ( pSettings );
	CSphString sWarning;
	ISphFilter * pRes = CreateFilter ( *pSettings, sAttrName, tSchema, nullptr, nullptr, sError, sWarning, true, SPH_COLLATION_DEFAULT, false );
	assert ( sWarning.IsEmpty() );
	return pRes;
}

ISphFilter * sphCreateFilter ( const KillListVector & dKillList )
{
	return new Filter_KillList ( dKillList );
}

ISphFilter * sphJoinFilters ( ISphFilter * pA, ISphFilter * pB )
{
	if ( pA )
		return pB ? pA->Join(pB) : pA;
	return pB;
}

static bool IsWeightColumn ( const CSphString & sAttr, const ISphSchema & tSchema )
{
	if ( sAttr=="@weight" )
		return true;

	const CSphColumnInfo * pCol = tSchema.GetAttr ( sAttr.cstr() );
	return ( pCol && pCol->m_bWeight );
}

CreateFilterContext_t::~CreateFilterContext_t()
{
	SafeDelete ( m_pFilter );
	SafeDelete ( m_pWeightFilter );

	for ( auto &pUserVal: m_dUserVals )
		pUserVal->Release();
}


static ISphFilter * CreateFilterNode ( CreateFilterContext_t & tCtx, int iNode, CSphString & sError, CSphString & sWarning, bool & bHasWeight )
{
	const FilterTreeItem_t * pCur = tCtx.m_pFilterTree->Begin() + iNode;
	if ( pCur->m_iFilterItem!=-1 )
	{
		const CSphFilterSettings * pFilterSettings = tCtx.m_pFilters->Begin() + pCur->m_iFilterItem;
		if ( pFilterSettings->m_sAttrName.IsEmpty() )
		{
			sError = "filter with empty name";
			return nullptr;
		}

		bHasWeight |= IsWeightColumn ( pFilterSettings->m_sAttrName, *tCtx.m_pSchema );

		// bind user variable local to that daemon
		CSphFilterSettings tUservar;
		if ( pFilterSettings->m_eType==SPH_FILTER_USERVAR )
		{
			const CSphString * sVar = pFilterSettings->m_dStrings.GetLength()==1 ? pFilterSettings->m_dStrings.Begin() : NULL;
			if ( !g_pUservarsHook || !sVar )
			{
				sError = "no global variables found";
				return nullptr;
			}

			const UservarIntSet_c * pUservar = g_pUservarsHook ( *sVar );
			if ( !pUservar )
			{
				sError.SetSprintf ( "undefined global variable '%s'", sVar->cstr() );
				return nullptr;
			}

			tCtx.m_dUserVals.Add ( pUservar );
			tUservar = *pFilterSettings;
			tUservar.m_eType = SPH_FILTER_VALUES;
			tUservar.SetExternalValues ( pUservar->Begin(), pUservar->GetLength() );
			pFilterSettings = &tUservar;
		}

		ISphFilter * pFilter = sphCreateFilter ( *pFilterSettings, tCtx, sError, sWarning );
		if ( !pFilter )
			return nullptr;

		return pFilter;
	}

	assert ( pCur->m_iLeft!=-1 && pCur->m_iRight!=-1 );
	ISphFilter * pLeft = CreateFilterNode ( tCtx, pCur->m_iLeft, sError, sWarning, bHasWeight );
	ISphFilter * pRight = CreateFilterNode ( tCtx, pCur->m_iRight, sError, sWarning, bHasWeight );

	if ( !pLeft || !pRight )
	{
		SafeDelete ( pLeft );
		SafeDelete ( pRight );
		return nullptr;
	}

	if ( pCur->m_bOr )
		return new Filter_Or ( pLeft, pRight );

	return new Filter_And2 ( pLeft, pRight, pLeft->UsesAttrs() || pRight->UsesAttrs() );
}


bool sphCreateFilters ( CreateFilterContext_t & tCtx, CSphString & sError, CSphString & sWarning )
{
	bool bGotFilters = ( tCtx.m_pFilters && tCtx.m_pFilters->GetLength() );
	bool bGotKlist = ( tCtx.m_pKillList && tCtx.m_pKillList->GetLength() );

	if ( !bGotFilters && !bGotKlist )
		return true;

	bool bGotTree = ( tCtx.m_pFilterTree && tCtx.m_pFilterTree->GetLength() );
	assert ( tCtx.m_pSchema );

	if ( bGotFilters )
	{
		if ( !bGotTree )
		{
			bool bScan = tCtx.m_bScan;
			for ( const CSphFilterSettings& dSettings  : (*tCtx.m_pFilters) )
			{
				if ( dSettings.m_sAttrName.IsEmpty() )
					continue;

				bool bWeight = IsWeightColumn ( dSettings.m_sAttrName, *tCtx.m_pSchema );
				if ( bScan && bWeight )
					continue; // @weight is not available in fullscan mode

				// bind user variable local to that daemon
				CSphFilterSettings tUservar;
				auto pFilterSettings = &dSettings;
				if ( dSettings.m_eType==SPH_FILTER_USERVAR )
				{
					const CSphString * sVar = dSettings.m_dStrings.GetLength()==1 ? dSettings.m_dStrings.Begin() : nullptr;
					if ( !g_pUservarsHook || !sVar )
					{
						sError = "no global variables found";
						return false;
					}

					const UservarIntSet_c * pUservar = g_pUservarsHook ( *sVar );
					if ( !pUservar )
					{
						sError.SetSprintf ( "undefined global variable '%s'", sVar->cstr() );
						return false;
					}

					tCtx.m_dUserVals.Add ( pUservar );
					tUservar = dSettings;
					tUservar.m_eType = SPH_FILTER_VALUES;
					tUservar.SetExternalValues ( pUservar->Begin(), pUservar->GetLength() );
					pFilterSettings = &tUservar;
				}

				ISphFilter * pFilter = sphCreateFilter ( *pFilterSettings, tCtx, sError, sWarning );
				if ( !pFilter )
					return false;

				ISphFilter ** pGroup = bWeight ? &tCtx.m_pWeightFilter : &tCtx.m_pFilter;
				*pGroup = sphJoinFilters ( *pGroup, pFilter );
			}
		} else
		{
			bool bWeight = false;
			ISphFilter * pFilter = CreateFilterNode ( tCtx, tCtx.m_pFilterTree->GetLength() - 1, sError, sWarning, bWeight );
			if ( !pFilter )
				return false;

			// weight filter phase only on match path
			if ( bWeight && tCtx.m_bScan )
				tCtx.m_pWeightFilter = pFilter;
			else
				tCtx.m_pFilter = pFilter;
		}
	}

	if ( bGotKlist )
	{
		ISphFilter * pFilter = sphCreateFilter ( *tCtx.m_pKillList );
		if ( !pFilter )
			return false;

		tCtx.m_pFilter = sphJoinFilters ( tCtx.m_pFilter, pFilter );
	}

	return true;
}

void FormatFilterQL ( const CSphFilterSettings & f, StringBuilder_c & tBuf, int iCompactIN )
{
	ScopedComma_c sEmpty (tBuf,nullptr); // disable outer scope separator
	switch ( f.m_eType )
	{
		case SPH_FILTER_VALUES:
			//tBuf << " " << f.m_sAttrName;
			tBuf << f.m_sAttrName;
			if ( f.m_dValues.GetLength()==1 )
				tBuf.Sprintf ( ( f.m_bExclude ? "!=%l" : "=%l" ), (int64_t)f.m_dValues[0] );
			else
			{
				ScopedComma_c tInComma ( tBuf, ",", ( f.m_bExclude ? " NOT IN (" : " IN (" ),")");
				if ( iCompactIN>0 && ( iCompactIN+1<f.m_dValues.GetLength() ) )
				{
					// for really long IN-lists optionally format them as N,N,N,N,...N,N,N, with ellipsis inside.
					int iLimit = Max ( iCompactIN-3, 3 );
					for ( int j=0; j<iLimit; ++j )
						tBuf.Sprintf ( "%l", (int64_t)f.m_dValues[j] );
					iLimit = f.m_dValues.GetLength();
					ScopedComma_c tElipsis ( tBuf, ",","...");
					for ( int j=iLimit-3; j<iLimit; ++j )
						tBuf.Sprintf ( "%l", (int64_t)f.m_dValues[j] );
				} else
					for ( int64_t iValue : f.m_dValues )
						tBuf.Sprintf ( "%l", iValue );
			}
			break;

		case SPH_FILTER_RANGE:
			if ( f.m_iMinValue==int64_t(INT64_MIN) || ( f.m_iMinValue==0 && f.m_sAttrName=="@id" ) )
			{
				// no min, thus (attr<maxval)
				const char * sOps[2][2] = { { "<", "<=" }, { ">=", ">" } };
				tBuf.Sprintf ( "%s%s%l", f.m_sAttrName.cstr(), sOps [ f.m_bExclude ][ f.m_bHasEqualMax ], f.m_iMaxValue );
			} else if ( f.m_iMaxValue==INT64_MAX || ( f.m_iMaxValue==-1 && f.m_sAttrName=="@id" ) )
			{
				// mo max, thus (attr>minval)
				const char * sOps[2][2] = { { ">", ">=" }, { "<", "<=" } };
				tBuf.Sprintf ( "%s%s%l", f.m_sAttrName.cstr(), sOps [ f.m_bExclude ][ f.m_bHasEqualMin ], f.m_iMinValue );
			} else
			{
				if ( f.m_bHasEqualMin!=f.m_bHasEqualMax)
				{
					const char * sOps[2]= { "<", "<=" };
					const char * sFmt = f.m_bExclude ? "NOT %l%s%s%s%l" : "%l%s%s%s%l";
					tBuf.Sprintf ( sFmt, f.m_iMinValue, sOps[f.m_bHasEqualMin], f.m_sAttrName.cstr(), sOps[f.m_bHasEqualMax], f.m_iMaxValue );
				} else
				{
					const char * sFmt = f.m_bExclude ? "%s NOT BETWEEN %l AND %l" : "%s BETWEEN %l AND %l";
					tBuf.Sprintf ( sFmt, f.m_sAttrName.cstr(), f.m_iMinValue + !f.m_bHasEqualMin, f.m_iMaxValue - !f.m_bHasEqualMax );
				}
			}
			break;

		case SPH_FILTER_FLOATRANGE:
			if ( f.m_fMinValue==-FLT_MAX )
			{
				// no min, thus (attr<maxval)
				const char * sOps[2][2] = { { "<", "<=" }, { ">=", ">" } };
				tBuf.Sprintf ( "%s%s%f", f.m_sAttrName.cstr(),
					sOps [ f.m_bExclude ][ f.m_bHasEqualMax ], f.m_fMaxValue );
			} else if ( f.m_fMaxValue==FLT_MAX )
			{
				// mo max, thus (attr>minval)
				const char * sOps[2][2] = { { ">", ">=" }, { "<", "<=" } };
				tBuf.Sprintf ( "%s%s%f", f.m_sAttrName.cstr(),
					sOps [ f.m_bExclude ][ f.m_bHasEqualMin ], f.m_fMinValue );
			} else
			{
				if ( f.m_bHasEqualMin!=f.m_bHasEqualMax)
				{
					const char * sOps[2]= { "<", "<=" };
					tBuf.Sprintf ( "%s%f%s%s%s%f", f.m_bExclude ? "NOT ": "", f.m_fMinValue, sOps[f.m_bHasEqualMin], f.m_sAttrName.cstr(), sOps[f.m_bHasEqualMax], f.m_fMaxValue );
				} else // FIXME? need we handle m_bHasEqual here?					
					tBuf.Sprintf ( "%s%s BETWEEN %f AND %f", f.m_sAttrName.cstr(), f.m_bExclude ? " NOT" : "",	f.m_fMinValue, f.m_fMaxValue );
			}
			break;

		case SPH_FILTER_USERVAR:
		case SPH_FILTER_STRING:
			tBuf.Sprintf ( "%s%s'%s'", f.m_sAttrName.cstr(), ( f.m_bExclude ? "!=" : "=" ), ( f.m_dStrings.GetLength()==1 ? f.m_dStrings[0].cstr() : "" ) );
			break;

		case SPH_FILTER_NULL:
			tBuf << f.m_sAttrName << ( f.m_bIsNull ? " IS NULL" : " IS NOT NULL" );
			break;

		case SPH_FILTER_STRING_LIST:

			tBuf << f.m_sAttrName;// << " IN (";
			if ( f.m_bExclude )
				tBuf << " NOT";
			if ( f.m_eMvaFunc==SPH_MVAFUNC_ANY )
				tBuf.StartBlock ( ", '", " ANY ('", "')" );
			else if ( f.m_eMvaFunc==SPH_MVAFUNC_ALL )
				tBuf.StartBlock ( ", '", " ALL ('", "')" );
			else
				tBuf.StartBlock ( ", '", " IN ('", "')" );
			for ( const auto &sString : f.m_dStrings )
				tBuf << sString;

			tBuf.FinishBlock ();
			break;

		case SPH_FILTER_EXPRESSION:
			tBuf << f.m_sAttrName;
			break;

		default:
			tBuf += "1 /""* oops, unknown filter type *""/";
			break;
	}
}

static CSphString LogFilterTreeItem ( int iItem, const CSphVector<FilterTreeItem_t> & dTree, const CSphVector<CSphFilterSettings> & dFilters, int iCompactIN )
{
	const FilterTreeItem_t & tItem = dTree[iItem];
	if ( tItem.m_iFilterItem!=-1 )
	{
		StringBuilder_c tBuf;
		FormatFilterQL ( dFilters[tItem.m_iFilterItem], tBuf, iCompactIN );
		return tBuf.cstr();// + iOff;
	}

	assert ( tItem.m_iLeft!=-1 && tItem.m_iRight!=-1 );
	CSphString sLeft = LogFilterTreeItem ( tItem.m_iLeft, dTree, dFilters, iCompactIN );
	CSphString sRight = LogFilterTreeItem ( tItem.m_iRight, dTree, dFilters, iCompactIN );

	bool bLeftPts = ( dTree[tItem.m_iLeft].m_iFilterItem==-1 && dTree[tItem.m_iLeft].m_bOr!=tItem.m_bOr );
	bool bRightPts = ( dTree[tItem.m_iRight].m_iFilterItem==-1 && dTree[tItem.m_iRight].m_bOr!=tItem.m_bOr );

	StringBuilder_c tBuf;
	if ( bLeftPts ) tBuf << "(" << sLeft << ")"; else tBuf << sLeft;
	tBuf << ( tItem.m_bOr ? " OR " : " AND " );
	if ( bRightPts ) tBuf << "(" << sRight << ")"; else tBuf << sRight;

	return tBuf.cstr();
}

void FormatFiltersQL ( const CSphVector<CSphFilterSettings> & dFilters, const CSphVector<FilterTreeItem_t> & dFilterTree,
	StringBuilder_c & tBuf, int iCompactIN )
{
	if ( dFilterTree.IsEmpty() )
	{
		ScopedComma_c sAND ( tBuf, " AND " );
		for ( const auto& dFilter : dFilters )
			FormatFilterQL ( dFilter, tBuf, iCompactIN );
	}
	else
		tBuf << LogFilterTreeItem ( dFilterTree.GetLength() - 1, dFilterTree, dFilters, iCompactIN );
}


class PercolateFilterValues_c : public PercolateFilter_i
{
public:
	explicit PercolateFilterValues_c ( const CSphFilterSettings & tUID )
		: m_pValues ( tUID.GetValueArray() )
		, m_iCount ( tUID.GetNumValues() )
	{}

	~PercolateFilterValues_c() final
	{}

	bool Eval ( SphAttr_t uUID ) final
	{
		if ( !m_pValues || !m_iCount )
			return true;

		return ( sphBinarySearch ( m_pValues, m_pValues + m_iCount - 1, SphIdentityFunctor_T<SphAttr_t>(), uUID )!=nullptr );
	}

private:
	const SphAttr_t * m_pValues;
	int m_iCount;
};

template < bool HAS_EQUAL_MIN, bool HAS_EQUAL_MAX, bool OPEN_LEFT, bool OPEN_RIGHT >
class PercolateFilterRange_c : public PercolateFilter_i
{
public:
	PercolateFilterRange_c () = default;

	~PercolateFilterRange_c() final
	{}

	bool Eval ( SphAttr_t uUID ) final
	{
		return EvalRange<HAS_EQUAL_MIN,HAS_EQUAL_MAX,OPEN_LEFT,OPEN_RIGHT> ( uUID, m_iMinValue, m_iMaxValue );
	}

	void SetRange ( SphAttr_t tMin, SphAttr_t tMax ) final
	{
		m_iMinValue = tMin;
		m_iMaxValue = tMax;
	}

	SphAttr_t m_iMinValue = 0;
	SphAttr_t m_iMaxValue = 0;
};

static PercolateFilter_i * CreatePercolateRangeFilter ( const CSphFilterSettings & tOpt )
{
	CREATE_RANGE_FILTER_WITH_OPEN ( PercolateFilterRange_c, tOpt );
}

PercolateFilter_i * CreatePercolateFilter ( const CSphFilterSettings * pUID )
{
	if ( !pUID )
		return nullptr;

	const CSphFilterSettings & tOpt = *pUID;

	if ( pUID->m_eType==SPH_FILTER_VALUES )
		return new PercolateFilterValues_c ( tOpt );

	PercolateFilter_i * pFilter = CreatePercolateRangeFilter ( tOpt );
	pFilter->SetRange ( tOpt.m_iMinValue, tOpt.m_iMaxValue );
	return pFilter;
}
