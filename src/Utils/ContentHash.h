#pragma once

#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <string_view>

/// SHA-256 (Windows CNG) based content hashing for shader cache keys.
/// Upstream (#2836/#2840) uses XXH3-128 from vcpkg's xxhash; this fork has no xxhash
/// package, so the same 128-bit key shape is filled from a truncated SHA-256 instead.
namespace Util::ContentHash
{
	struct Hash128
	{
		uint64_t high = 0;
		uint64_t low = 0;

		bool operator==(const Hash128&) const = default;

		/** @brief 32 lowercase hex digits, high word first. */
		std::string ToHex() const
		{
			return std::format("{:016x}{:016x}", high, low);
		}
	};

	/// Incremental SHA-256. One instance per thread; the algorithm provider is shared (CNG allows that).
	class Sha256
	{
	public:
		Sha256()
		{
			if (const auto provider = Provider())
				valid = BCRYPT_SUCCESS(BCryptCreateHash(provider, &hash, nullptr, 0, nullptr, 0, 0));
		}
		~Sha256()
		{
			if (hash)
				BCryptDestroyHash(hash);
		}
		Sha256(const Sha256&) = delete;
		Sha256& operator=(const Sha256&) = delete;

		void Update(const void* a_data, size_t a_size)
		{
			// BCryptHashData takes a ULONG length, so feed very large inputs in chunks.
			auto bytes = static_cast<const uint8_t*>(a_data);
			while (valid && a_size > 0) {
				const auto chunk = static_cast<ULONG>((std::min)(a_size, static_cast<size_t>(1u << 30)));
				valid = BCRYPT_SUCCESS(BCryptHashData(hash, const_cast<PUCHAR>(bytes), chunk, 0));
				bytes += chunk;
				a_size -= chunk;
			}
		}
		void Update(std::string_view a_text) { Update(a_text.data(), a_text.size()); }

		/// Length-prefixed field, so ("ab","c") and ("a","bc") hash differently.
		void UpdateField(std::string_view a_text)
		{
			const uint64_t size = a_text.size();
			Update(&size, sizeof(size));
			Update(a_text);
		}

		/// @return false when CNG failed at any step; the digest is then unusable.
		bool Finish(std::array<uint8_t, 32>& a_digest)
		{
			if (!valid)
				return false;
			valid = BCRYPT_SUCCESS(BCryptFinishHash(hash, a_digest.data(), static_cast<ULONG>(a_digest.size()), 0));
			return valid;
		}

		/// First 128 bits of the digest; a zero hash when CNG failed.
		Hash128 Finish128()
		{
			std::array<uint8_t, 32> digest{};
			if (!Finish(digest))
				return {};
			Hash128 result;
			std::memcpy(&result.high, digest.data(), sizeof(uint64_t));
			std::memcpy(&result.low, digest.data() + sizeof(uint64_t), sizeof(uint64_t));
			return result;
		}

		bool IsValid() const { return valid; }

	private:
		static BCRYPT_ALG_HANDLE Provider()
		{
			static const BCRYPT_ALG_HANDLE provider = [] {
				BCRYPT_ALG_HANDLE handle = nullptr;
				return BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) ? handle : nullptr;
			}();
			return provider;
		}

		BCRYPT_HASH_HANDLE hash = nullptr;
		bool valid = false;
	};

	inline Hash128 HashBytes(const void* a_data, size_t a_size)
	{
		Sha256 sha;
		sha.Update(a_data, a_size);
		return sha.Finish128();
	}

	inline Hash128 HashString(std::string_view a_text)
	{
		return HashBytes(a_text.data(), a_text.size());
	}

	/// Order-sensitive combination of two hashes.
	inline Hash128 CombineHashes(const Hash128& a_first, const Hash128& a_second)
	{
		const std::array<uint64_t, 4> buffer{ a_first.high, a_first.low, a_second.high, a_second.low };
		return HashBytes(buffer.data(), buffer.size() * sizeof(uint64_t));
	}
}
