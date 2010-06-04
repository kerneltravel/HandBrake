// --------------------------------------------------------------------------------------------------------------------
// <copyright file="Subtitle.cs" company="HandBrake Project (http://handbrake.fr)">
//   This file is part of the HandBrake source code - It may be used under the terms of the GNU General Public License.
// </copyright>
// <summary>
//   An object that represents a subtitle associated with a Title, in a DVD
// </summary>
// --------------------------------------------------------------------------------------------------------------------

namespace HandBrake.Interop.SourceData
{
    /// <summary>
    /// An object that represents a subtitle associated with a Title, in a DVD
    /// </summary>
    public class Subtitle
    {
        /// <summary>
        /// The track number of this Subtitle
        /// </summary>
        public int TrackNumber { get; set; }

        /// <summary>
        /// The language (if detected) of this Subtitle
        /// </summary>
        public string Language { get; set; }

        /// <summary>
        /// Langauage Code
        /// </summary>
        public string LanguageCode { get; set; }

        public SubtitleType SubtitleType { get; set; }

        /// <summary>
        /// Subtitle Type
        /// </summary>
        public string TypeString
        {
            get
            {
                if (this.SubtitleType == SubtitleType.Picture)
                {
                    return "Bitmap";
                }
                else
                {
                    return "Text";
                }
            }
        }

        /// <summary>
        /// Override of the ToString method to make this object easier to use in the UI
        /// </summary>
        /// <returns>A string formatted as: {track #} {language}</returns>
        public override string ToString()
        {
            return string.Format("{0} {1} ({2})", this.TrackNumber, this.Language, this.TypeString);
        }

        public string Display
        {
            get
            {
                return this.ToString();
            }
        }
    }
}